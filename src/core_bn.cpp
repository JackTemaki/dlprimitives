#include <dlprim/core_ops.hpp>
#include <dlprim/gpu/program_cache.hpp>

namespace dlprim {
namespace core {
    class BatchNorm2DImpl : public BatchNorm2DFwdBwd {
    public:
        virtual ~BatchNorm2DImpl() {}

        BatchNorm2DImpl(Context &ctx,Shape const &s,DataType dtype)
        {
            dt_ = dtype;
            DLPRIM_CHECK(dtype == float_data);
            DLPRIM_CHECK(s.size() == 4);
            int total = s[0] * s[2] * s[3];
            features_ = s[1];
            int second_size = (total + 255) / 256;
            ws_ = features_ * size_of_data_type(dtype) * 5; // two sums + fdy+ fdx+ off
            if(second_size < 64) {
                if(total >= 256)
                    wg_ = 256;
                else if(total >= 128)
                    wg_ = 128;
                else
                    wg_ = 64;
                second_reduce_ = 1;
            }
            else {
                wg_ = 256;
                if(second_size >= 256)
                    second_reduce_ = 256;
                else if(second_size >= 128)
                    second_reduce_ = 128;
                else
                    second_reduce_ = 64;
                ws_ += second_reduce_ * 2 * size_of_data_type(dtype) * features_;
            }
            
            cl::Program const &fwd_sums = gpu::Cache::instance().get_program(ctx,
                        "bn_sums",
                        "WG_SIZE",wg_,
                        "BACKWARD",0,
                        "SECOND_REDUCE_SIZE",second_reduce_);

            cl::Program const &bwd_sums = gpu::Cache::instance().get_program(ctx,
                        "bn_sums",
                        "WG_SIZE",wg_,
                        "BACKWARD",1,
                        "SECOND_REDUCE_SIZE",second_reduce_);

            cl::Program const &utils = gpu::Cache::instance().get_program(ctx,"bn_utils");

            sums_ = cl::Kernel(fwd_sums,"compute");
            if(second_reduce_ > 1)
                sums_reduce_ = cl::Kernel(fwd_sums,"reduce");
            dyx_sums_ = cl::Kernel(bwd_sums,"compute");
            if(second_reduce_)
                dyx_sums_reduce_ = cl::Kernel(bwd_sums,"reduce");

            
            update_sums_ = cl::Kernel(utils,"update_sums");
            mean_var_to_a_b_ =cl::Kernel(utils,"mean_var_to_a_b");
            combine_mean_var_with_gamma_beta_ = cl::Kernel(utils,"combine_mean_var_with_gamma_beta");
            compute_backward_factors_ = cl::Kernel(utils,"compute_backward_factors");
            forward_ = cl::Kernel(utils,"forward");
            backward_data_ = cl::Kernel(utils,"backward_data");
            backward_filter_ = cl::Kernel(utils,"backward_filter");
            var_gamma_to_a_ = cl::Kernel(utils,"var_gamma_to_a");
            backward_test_ = cl::Kernel(utils,"backward_test");
        }
        ///
        /// Workspace size needed for intermediate results of computations
        ///
        virtual size_t workspace()
        {
            return ws_;
        }

        
        virtual void enqueue_calculate_batch_stats(Tensor &x,Tensor &mean,Tensor &var,Tensor &ws,ExecutionContext const &e)
        {
            int batch = x.shape()[0];
            int channels = x.shape()[1];
            DLPRIM_CHECK(channels==features_);
            int hw = x.shape()[2] * x.shape()[3];
            int p = 0;
            sums_.setArg(p++,batch);
            sums_.setArg(p++,channels);
            sums_.setArg(p++,hw);
            x.set_arg(sums_,p);
            if(second_reduce_ <= 1) {
                mean.set_arg(sums_,p);
                var.set_arg(sums_,p);
                e.queue().enqueueNDRangeKernel( sums_,
                                                cl::NullRange,
                                                cl::NDRange(wg_,features_),
                                                cl::NDRange(wg_,1),
                                                e.events(),e.event("calc_mean_var"));
            }
            else {
                Tensor x_sum = ws.sub_tensor(0,Shape(second_reduce_,features_),dt_);
                Tensor x2_sum = ws.sub_tensor(second_reduce_ * features_ * size_of_data_type(dt_)/size_of_data_type(ws.dtype()),
                                              Shape(second_reduce_,features_),dt_);
                sums_.setArg(p++,x_sum.device_buffer());
                sums_.setArg(p++,int(x_sum.device_offset()));
                sums_.setArg(p++,x2_sum.device_buffer());
                sums_.setArg(p++,int(x2_sum.device_offset()));
                p=0;
                sums_reduce_.setArg(p++,x_sum.device_buffer());
                sums_reduce_.setArg(p++,int(x_sum.device_offset()));
                sums_reduce_.setArg(p++,x2_sum.device_buffer());
                sums_reduce_.setArg(p++,int(x2_sum.device_offset()));
                sums_reduce_.setArg(p++,mean.device_buffer());
                sums_reduce_.setArg(p++,int(mean.device_offset()));
                sums_reduce_.setArg(p++,var.device_buffer());
                sums_reduce_.setArg(p++,int(var.device_offset()));
                sums_reduce_.setArg(p++,1.0f/(batch*hw));

                auto e1 = e.generate_series_context(0,2);
                auto e2 = e.generate_series_context(1,2);
                e.queue().enqueueNDRangeKernel( sums_,
                                                cl::NullRange,
                                                cl::NDRange(wg_,features_),
                                                cl::NDRange(wg_*second_reduce_,1),
                                                e1.events(),e1.event("calc_mean_var"));

                e.queue().enqueueNDRangeKernel( sums_reduce_,
                                                cl::NullRange,
                                                cl::NDRange(second_reduce_,features_),
                                                cl::NDRange(second_reduce_,1),
                                                e2.events(),e2.event("calc_mean_var_reduce"));
            }
        }
        
        virtual void enqueue_update_running_stats(float batch_mean_factor,float running_mean_factor,
                                                  Tensor &batch_mean,Tensor &running_mean,
                                                  float batch_var_factor,float running_var_factor,
                                                  Tensor &batch_var,Tensor &running_var,
                                                  Tensor &ws,ExecutionContext const &e)
        {
            int p=0;
            update_sums_.setArg(p++,batch_mean.device_buffer());
            update_sums_.setArg(p++,int(batch_mean.device_offset()));
            update_sums_.setArg(p++,batch_var.device_buffer());
            update_sums_.setArg(p++,int(batch_var.device_offset()));
            update_sums_.setArg(p++,running_mean.device_buffer());
            update_sums_.setArg(p++,int(running_mean.device_offset()));
            update_sums_.setArg(p++,running_var.device_buffer());
            update_sums_.setArg(p++,int(running_var.device_offset()));
            update_sums_.setArg(p++,batch_mean_factor);
            update_sums_.setArg(p++,running_mean_factor);
            update_sums_.setArg(p++,batch_var_factor);
            update_sums_.setArg(p++,running_var_factor);
            e.queue().enqueueNDRangeKernel(update_sums_,
                                           cl::NullRange,cl::NDRange(features_),cl::NullRange,
                                           e.events(),e.event("update_sums"));
        }

        ///
        /// Peform forward computation as y = (x-mean) / sqrt(var + eps)
        ///
        /// Note mean/var can be taken from batch or from global running stats as per user request
        ///
        void forward_ab(Tensor &x,Tensor &y,Tensor &a,Tensor &b,ExecutionContext const &e)
        {
            int p = 0;
            int batches = x.shape()[0];
            int rc = x.shape()[2]*x.shape()[3];
            forward_.setArg(p++,int(x.shape()[0]));
            forward_.setArg(p++,features_);
            forward_.setArg(p++,int(x.shape()[2]*x.shape()[3]));

            x.set_arg(forward_,p);
            y.set_arg(forward_,p);
            a.set_arg(forward_,p);
            b.set_arg(forward_,p);
            e.queue().enqueueNDRangeKernel(forward_,cl::NullRange,cl::NDRange(batches,features_,rc),cl::NullRange,e.events(),e.event("forward"));
        }

        void split_ws_to_a_b_rest(Tensor &ws,Tensor &a,Tensor &b,Tensor &rest)
        {
            split_ws_to_a_b(ws,a,b);
            size_t diff = size_of_data_type(dt_) * 2 / size_of_data_type(ws.dtype());
            size_t res_size = ws.shape().total_size() - diff;
            rest = ws.sub_tensor(diff,Shape(res_size),ws.dtype());
        }
        void split_ws_to_a_b(Tensor &ws,Tensor &a,Tensor &b)
        {
            a = ws.sub_tensor_target_offset(0,Shape(features_),dt_);
            b = ws.sub_tensor_target_offset(features_,Shape(features_),dt_);
        }
        virtual void enqueue_forward_direct(Tensor &x,Tensor &y,
                                            Tensor &mean,Tensor &var,float eps,
                                            Tensor &ws,ExecutionContext const &e)
        {
            Tensor a,b;
            split_ws_to_a_b(ws,a,b);

            int p = 0;
            mean_var_to_a_b_.setArg(p++,features_);
            mean_var_to_a_b_.setArg(p++,eps);

            mean.set_arg(mean_var_to_a_b_,p);
            var.set_arg(mean_var_to_a_b_,p);
            a.set_arg(mean_var_to_a_b_,p);
            b.set_arg(mean_var_to_a_b_,p);

            auto e1=e.generate_series_context(0,2);
            auto e2=e.generate_series_context(1,2);
            e.queue().enqueueNDRangeKernel(mean_var_to_a_b_,cl::NullRange,cl::NDRange(features_),cl::NullRange,e1.events(),e1.event("mean_to_ab"));
            forward_ab(x,y,a,b,e2);

        }
        ///
        /// Peform forward computation as y = (x-mean) / sqrt(var + eps) * gamma + beta 
        ///
        /// Notes:
        /// - mean/var can be taken from batch or from global running stats as per user request
        /// - mean/var and gamma/beta are converted to single y=ax+b and than computation is done in a single step
        ///
        virtual void enqueue_forward_affine(Tensor &x,Tensor &y,
                                            Tensor &gamma,Tensor &beta,
                                            Tensor &mean,Tensor &var,
                                            float eps,
                                            Tensor &ws,ExecutionContext const &e)
        {
            Tensor a,b;
            split_ws_to_a_b(ws,a,b);

            int p = 0;
            combine_mean_var_with_gamma_beta_.setArg(p++,features_);
            combine_mean_var_with_gamma_beta_.setArg(p++,eps);

            mean.set_arg(combine_mean_var_with_gamma_beta_,p);
            var.set_arg(combine_mean_var_with_gamma_beta_,p);
            gamma.set_arg(combine_mean_var_with_gamma_beta_,p);
            beta.set_arg(combine_mean_var_with_gamma_beta_,p);
            a.set_arg(combine_mean_var_with_gamma_beta_,p);
            b.set_arg(combine_mean_var_with_gamma_beta_,p);

            auto e1=e.generate_series_context(0,2);
            auto e2=e.generate_series_context(1,2);
            e.queue().enqueueNDRangeKernel(combine_mean_var_with_gamma_beta_,cl::NullRange,cl::NDRange(features_),cl::NullRange,e1.events(),e1.event("mean_gamma_to_ab"));
            forward_ab(x,y,a,b,e2);
        }

        ///
        /// Perform backpropogation calculations
        ///
        /// training_mode - assumes that mean/var were calculated on batches of X - they need to be kept from forward stage
        ///   otherwise mean/var considered constant values
        ///
        /// gamma/beta affine transofrmation after BN
        ///
        /// dy - top gradient for backpropogation
        /// dx - calculate backpropogation on X
        /// dgamma - calculate backpropogation gradient for gamma scale
        /// dbeta - calculate backpropogation gradient for beta scale
        /// ws - worksspace 
        ///
        virtual void enqueue_backward_affine(bool training_mode,
                                             Tensor &x,Tensor &dy,
                                             Tensor &mean,Tensor &var,
                                             Tensor &gamma,Tensor &beta,
                                             Tensor *dx,float dx_factor,
                                             Tensor *dgamma,float dgamma_factor,
                                             Tensor *dbeta,float dbeta_factor,
                                             float eps,
                                             Tensor &ws,ExecutionContext const &e)
        {
            if(!dx && !dgamma && !dbeta)
                return;
            Tensor dyx_sum,dy_sum,new_ws;
            split_ws_to_a_b_rest(ws,dyx_sum,dy_sum,new_ws);
            int N = 1 + int(dgamma || dbeta) + (dx != nullptr);
            int id = 0;
            calc_sums(x,dy,new_ws,dyx_sum,dy_sum,e.generate_series_context(id++,N));
            if(dgamma || dbeta)
                backward_filter(mean,var,dyx_sum,dy_sum,
                                (dgamma ? *dgamma : null_),(dbeta ? *dbeta : null_),
                                dgamma_factor,dbeta_factor,eps,e.generate_series_context(id++,N));
            if(dx) {
                auto e2 = e.generate_series_context(id++,N);
                if(training_mode) {
                    backward_data_train(*dx,dy,mean,var,dy_sum,dyx_sum,gamma,new_ws,eps,dx_factor,e2);
                }
                else {
                    backward_data_test(*dx,dy,var,gamma,new_ws,eps,dx_factor,e2);
                }
            }

        }

        void calc_sums(Tensor &x,Tensor &dy,Tensor &ws,Tensor &dyx_sum,Tensor &dy_sum,ExecutionContext const &e)
        {
            int batch = x.shape()[0];
            int channels = x.shape()[1];
            DLPRIM_CHECK(channels==features_);
            int hw = x.shape()[2] * x.shape()[3];
            int p = 0;
            dyx_sums_.setArg(p++,batch);
            dyx_sums_.setArg(p++,channels);
            dyx_sums_.setArg(p++,hw);
            x.set_arg(dyx_sums_,p);
            dy.set_arg(dyx_sums_,p);
            if(second_reduce_ <= 1) {
                dyx_sum.set_arg(dyx_sums_,p);
                dy_sum.set_arg(dyx_sums_,p);
                e.queue().enqueueNDRangeKernel( dyx_sums_,
                                                cl::NullRange,
                                                cl::NDRange(wg_,features_),
                                                cl::NDRange(wg_,1),
                                                e.events(),e.event("calc_dyx_sums"));
            }
            else {
                Tensor s1 = ws.sub_tensor(0,Shape(second_reduce_,features_),dt_);
                Tensor s2 = ws.sub_tensor_target_offset(features_,
                                              Shape(second_reduce_,features_),dt_);
                s1.set_arg(dyx_sums_,p);
                s2.set_arg(dyx_sums_,p);
                p=0;
                s1.set_arg(dyx_sums_reduce_,p);
                s2.set_arg(dyx_sums_reduce_,p);
                dyx_sum.set_arg(dyx_sums_reduce_,p);
                dy_sum.set_arg(dyx_sums_reduce_,p);

                auto e1 = e.generate_series_context(0,2);
                auto e2 = e.generate_series_context(1,2);
                e.queue().enqueueNDRangeKernel( dyx_sums_,
                                                cl::NullRange,
                                                cl::NDRange(wg_,features_),
                                                cl::NDRange(wg_*second_reduce_,1),
                                                e1.events(),e1.event("calc_dyx_x"));

                e.queue().enqueueNDRangeKernel( dyx_sums_reduce_,
                                                cl::NullRange,
                                                cl::NDRange(second_reduce_,features_),
                                                cl::NDRange(second_reduce_,1),
                                                e2.events(),e2.event("calc_dyx_x_reduce"));
            }
        }

        void backward_data_test(Tensor &dx,Tensor &dy,Tensor &var,Tensor &gamma,
                                Tensor &ws,float eps,float dx_factor,ExecutionContext const &e)
        {
            auto e1 = e.generate_series_context(0,2);
            auto e2 = e.generate_series_context(1,2);

            Tensor  dy_factor = ws.sub_tensor_target_offset(0*features_,Shape(features_),dt_);
            int batches = dx.shape()[0];
            int hw = dx.shape()[2]*dx.shape()[3];
            int p=0;
            var_gamma_to_a_.setArg(p++,features_);
            var_gamma_to_a_.setArg(p++,eps);
            var.set_arg(var_gamma_to_a_,p);
            gamma.set_arg(var_gamma_to_a_,p);
            dy_factor.set_arg(var_gamma_to_a_,p);
            e.queue().enqueueNDRangeKernel(var_gamma_to_a_,
                                           cl::NullRange,cl::NDRange(features_),cl::NullRange,
                                           e1.events(),e2.event("var_gamma_to_a"));
            p=0;
            backward_test_.setArg(p++,batches);
            backward_test_.setArg(p++,features_);
            backward_test_.setArg(p++,hw);
            dx.set_arg(backward_test_,p);
            dy.set_arg(backward_test_,p);
            dy_factor.set_arg(backward_test_,p);
            backward_test_.setArg(p++,dx_factor);
            e.queue().enqueueNDRangeKernel(backward_test_,
                                           cl::NullRange,cl::NDRange(batches,features_,hw),cl::NullRange,
                                           e2.events(),e2.event("backward_data"));
        }

        void backward_data_train(Tensor &dx,Tensor &dy,
                                 Tensor &mean,Tensor &var,
                                 Tensor &dy_sum,Tensor &dyx_sum,
                                 Tensor &gamma,Tensor &ws,
                                 float eps,float scale,ExecutionContext const &e)
        {
            auto e1 = e.generate_series_context(0,2);
            auto e2 = e.generate_series_context(1,2);

            Tensor  x_factor = ws.sub_tensor_target_offset(0*features_,Shape(features_),dt_);
            Tensor dy_factor = ws.sub_tensor_target_offset(1*features_,Shape(features_),dt_);
            Tensor  b_offset = ws.sub_tensor_target_offset(2*features_,Shape(features_),dt_);
            int batches = dx.shape()[0];
            int hw = dx.shape()[2]*dx.shape()[3];
            int total = batches*hw;
            int p=0;
            compute_backward_factors_.setArg(p++,features_);
            compute_backward_factors_.setArg(p++,total);
            compute_backward_factors_.setArg(p++,eps);
            mean.set_arg(compute_backward_factors_,p);
            var.set_arg(compute_backward_factors_,p);
            dy_sum.set_arg(compute_backward_factors_,p);
            dyx_sum.set_arg(compute_backward_factors_,p);
            gamma.set_arg(compute_backward_factors_,p);
            x_factor.set_arg(compute_backward_factors_,p);
            dy_factor.set_arg(compute_backward_factors_,p);
            b_offset.set_arg(compute_backward_factors_,p);

            e.queue().enqueueNDRangeKernel(compute_backward_factors_,
                                   cl::NullRange,cl::NDRange(features_),cl::NullRange,
                                   e1.events(),e1.event("compute_backward_factors"));
            p=0;
            backward_data_.setArg(p++,batches);
            backward_data_.setArg(p++,features_);
            backward_data_.setArg(p++,hw);
            dx.set_arg(backward_data_,p);
            dy.set_arg(backward_data_,p);
            x_factor.set_arg(backward_data_,p);
            dy_factor.set_arg(backward_data_,p);
            b_offset.set_arg(backward_data_,p);
            dx.set_arg(backward_data_,p);
            backward_data_.setArg(p++,scale);
            e.queue().enqueueNDRangeKernel(backward_data_,
                                           cl::NullRange,cl::NDRange(batches,features_,hw),
                                           cl::NullRange,e2.events(),e2.event("backward_data"));
        }

        void backward_filter(Tensor &mean,Tensor &var,Tensor &dyx_sum,Tensor &dy_sum,
                             Tensor &dgamma,Tensor &dbeta,
                             float dg_fact,float db_fact,float eps,ExecutionContext const &e)
        {
            int p=0;
            backward_filter_.setArg(p,features_);
            mean.set_arg(backward_filter_,p);
            var.set_arg(backward_filter_,p);
            dy_sum.set_arg(backward_filter_,p);
            dyx_sum.set_arg(backward_filter_,p);
            dgamma.set_arg(backward_filter_,p);
            dbeta.set_arg(backward_filter_,p);
            backward_filter_.setArg(p++,eps);
            backward_filter_.setArg(p++,dg_fact);
            backward_filter_.setArg(p++,db_fact);
            e.queue().enqueueNDRangeKernel(backward_filter_,cl::NullRange,cl::NDRange(features_),cl::NullRange,
                        e.events(),e.event("backward_filter"));
        }

        ///
        /// Perform backpropogation calculations for BN without affine addtition Gamma/Beta
        ///
        /// training_mode - assumes that mean/var were calculated on batches of X - they need to be kept from forward stage
        ///   otherwise mean/var considered constant values
        ///
        /// dy - top gradient for backpropogation
        /// dx - calculate backpropogation on X 
        /// ws - worksspace 
        ///
        virtual void enqueue_backward_direct(bool training_mode,
                                             Tensor &x,Tensor &dy,
                                             Tensor &mean,Tensor &var,
                                             Tensor &dx,float dx_factor,
                                             float eps,
                                             Tensor &ws,ExecutionContext const &e)
        {
            Tensor dyx_sum,dy_sum,new_ws;
            split_ws_to_a_b_rest(ws,dyx_sum,dy_sum,new_ws);
            calc_sums(x,dy,new_ws,dyx_sum,dy_sum,e.generate_series_context(0,2));
            auto e2 = e.generate_series_context(1,2);
            if(training_mode) {
                backward_data_train(dx,dy,mean,var,dy_sum,dyx_sum,null_,new_ws,eps,dx_factor,e2);
            }
            else {
                backward_data_test(dx,dy,var,null_,new_ws,eps,dx_factor,e2);
            }
        }
    private:
        int features_;
        int ws_;
        int wg_;
        int second_reduce_;
        DataType dt_;
        cl::Kernel sums_,sums_reduce_;
        cl::Kernel dyx_sums_,dyx_sums_reduce_;
        cl::Kernel forward_,backward_data_,backward_filter_;
        cl::Kernel update_sums_;
        cl::Kernel mean_var_to_a_b_;
        cl::Kernel compute_backward_factors_;
        cl::Kernel combine_mean_var_with_gamma_beta_;
        cl::Kernel var_gamma_to_a_;
        cl::Kernel backward_test_;

        Tensor null_;
    };

    std::unique_ptr<BatchNorm2DFwdBwd> BatchNorm2DFwdBwd::create(Context &ctx,Shape const &s,DataType dt)
    {
        std::unique_ptr<BatchNorm2DFwdBwd> r(new BatchNorm2DImpl(ctx,s,dt));
        return r;
    }

} // core_ops
} // dlprim
