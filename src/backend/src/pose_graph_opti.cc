#include <backend/header.h>

#include "g2o/core/robust_kernel_impl.h"
#include "g2o/core/block_solver.h"
#include "g2o/core/optimization_algorithm_levenberg.h"
#include "g2o/solvers/linear_solver_eigen.h"
#include "g2o/types/types_seven_dof_expmap.h"
#include "opencv2/opencv.hpp"
#include <glog/logging.h>
#include <gflags/gflags.h>

DEFINE_int32(opti_count, 100, "How many of the iteration of optimization");
DEFINE_double(gps_weight, 0.0001, "The weight of GPS impact in optimization");
DEFINE_double(t_c_g_x, 0, "gps position in camera coordinate");
DEFINE_double(t_c_g_y, 0, "gps position in camera coordinate");
DEFINE_double(t_c_g_z, 0, "gps position in camera coordinate");

namespace g2o {
    class EdgePosiPreSim3 : public BaseUnaryEdge<3, Eigen::Vector3d, VertexSim3Expmap>{
    public:
        EIGEN_MAKE_ALIGNED_OPERATOR_NEW;
        EdgePosiPreSim3(){};

        bool read(std::istream& is){return true;};

        bool write(std::ostream& os) const{return true;};

        void computeError()  {
        const g2o::VertexSim3Expmap* v1 = static_cast<const VertexSim3Expmap*>(_vertices[0]);
        _error= v1->estimate().inverse().translation()-_measurement;
        //std::cout<<v1->estimate().inverse().translation().transpose()<<std::endl;
        //std::cout<<v1->estimate().scale()<<std::endl;
        //std::cout<<v1->estimate().inverse().translation().transpose()/v1->estimate().scale()<<std::endl;
        }
    };
}

void pose_graph_opti(std::shared_ptr<gm::GlobalMap> map_p){
    g2o::SparseOptimizer optimizer;
    optimizer.setVerbose(false);
    
    g2o::OptimizationAlgorithmLevenberg* solver = new g2o::OptimizationAlgorithmLevenberg(
        new g2o::BlockSolver_7_3(
            new g2o::LinearSolverEigen<g2o::BlockSolver_7_3::PoseMatrixType>()));

    solver->setUserLambdaInit(1e-16);
    optimizer.setAlgorithm(solver);
    
    std::vector<g2o::VertexSim3Expmap*> v_sim3_list;
    std::vector<g2o::Sim3> sim3_list;
    
    std::map<std::shared_ptr<gm::Frame>, g2o::VertexSim3Expmap*> frame_to_vertex;
    std::map<g2o::VertexSim3Expmap*, std::shared_ptr<gm::Frame>> vertex_to_frame;
    
    for(int i=0; i<map_p->frames.size(); i++){
        g2o::VertexSim3Expmap* VSim3 = new g2o::VertexSim3Expmap();
        Eigen::Matrix4d pose_temp = map_p->frames[i]->getPose();
        pose_temp.block(0,3,3,1)=map_p->frames[i]->gps_position;
        
        Eigen::Matrix4d pose_inv=pose_temp.inverse();

        Eigen::Matrix<double,3,3> Rcw = pose_inv.block(0,0,3,3);
        Eigen::Matrix<double,3,1> tcw = pose_inv.block(0,3,3,1);
        g2o::Sim3 Siw;
        double scale=Rcw.block(0,0,3,1).norm();
        Rcw=Rcw/scale;
        Siw=g2o::Sim3(Rcw,tcw,scale);
        
        VSim3->setEstimate(Siw);
        VSim3->setFixed(false);
        
        VSim3->setId(i);
        VSim3->setMarginalized(false);
        VSim3->_fix_scale = false;

        optimizer.addVertex(VSim3);
        v_sim3_list.push_back(VSim3);
        sim3_list.push_back(Siw);
        frame_to_vertex[map_p->frames[i]]=VSim3;
        vertex_to_frame[VSim3]=map_p->frames[i];
    }

    std::vector<g2o::EdgePosiPreSim3*> gps_edges;
    for(int i=0; i<map_p->frames.size(); i++){
        if(map_p->frames[i]->gps_accu<30){
            g2o::EdgePosiPreSim3* e = new g2o::EdgePosiPreSim3();
            //std::cout<<v_sim3_list[i]->estimate().inverse().translation().transpose()<<std::endl;
            e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(frame_to_vertex[map_p->frames[i]]));
            e->setMeasurement(map_p->frames[i]->gps_position);
            Eigen::Matrix<double, 3, 3> con_mat = Eigen::Matrix<double, 3, 3>::Identity()*map_p->frames[i]->gps_accu*FLAGS_gps_weight;
            con_mat(2,2)=0.0001;
            e->information()=con_mat;
            optimizer.addEdge(e);
            gps_edges.push_back(e);
            //e->computeError();
        }
    }
    std::cout<<"add gps edge: "<<gps_edges.size()<<std::endl;
    
    std::vector<g2o::EdgeSim3*> sim3_edge_list;
    for(int i=0; i<map_p->pose_graph_v1.size(); i++){
        g2o::Sim3 Sji(map_p->pose_graph_e_rot[i],map_p->pose_graph_e_posi[i],map_p->pose_graph_e_scale[i]);
        g2o::EdgeSim3* e = new g2o::EdgeSim3();
        //std::cout<<"v1: "<<v_sim3_list[graph_v1_list[i]]->estimate().inverse().translation().transpose()<<std::endl;
        //std::cout<<"obs: "<<(v_sim3_list[graph_v2_list[i]]->estimate().inverse().rotation().toRotationMatrix()*Sji.translation()+v_sim3_list[graph_v2_list[i]]->estimate().inverse().translation()).transpose()<<std::endl;
        e->setVertex(0, dynamic_cast<g2o::OptimizableGraph::Vertex*>(frame_to_vertex[map_p->pose_graph_v1[i]]));
        e->setVertex(1, dynamic_cast<g2o::OptimizableGraph::Vertex*>(frame_to_vertex[map_p->pose_graph_v2[i]]));
        e->setMeasurement(Sji);
        //std::cout<<map_p->pose_graph_weight[i]<<std::endl;
        Eigen::Matrix<double,7,7> matLambda = Eigen::Matrix<double,7,7>::Identity()*map_p->pose_graph_weight[i];
        e->information() = matLambda;
        //e->computeError();
        //std::cout<<sqrt(e->chi2())<<std::endl;
        
//             if(input_is_sim || graph_weight[i]>0){
            optimizer.addEdge(e);
            sim3_edge_list.push_back(e);
//             }else{
//                 e->computeError();
//                 if(sqrt(e->chi2())<2){
//                     optimizer.addEdge(e);
//                     sim3_edge_list.push_back(e);
//                 }
//             }
    }
    std::cout<<"add sim3 edge: "<<sim3_edge_list.size()<<std::endl;
    
    float avg_error=0;
    for(int i=0; i<gps_edges.size(); i++){
        gps_edges[i]->computeError();
        avg_error=avg_error+sqrt(gps_edges[i]->chi2())/gps_edges.size();
    }
    std::cout<<"gps edge err before: "<<avg_error<<std::endl;
    avg_error=0;
    for(int i=0; i<sim3_edge_list.size(); i++){
        sim3_edge_list[i]->computeError();
        avg_error=avg_error+sqrt(sim3_edge_list[i]->chi2())/sim3_edge_list.size();
//             if(sqrt(sim3_edge_list[i]->chi2())>1){
//                 std::cout<<"avg_error: "<<avg_error<<"||"<<sqrt(sim3_edge_list[i]->chi2())<<std::endl;
//             }
        if(avg_error<0){
            return;
        }
    }
    std::cout<<"sim3 edge err before: "<<avg_error<<std::endl;
    
    optimizer.initializeOptimization();
    //optimizer.computeInitialGuess();
    optimizer.optimize(FLAGS_opti_count);
    
    avg_error=0;
    for(int i=0; i<gps_edges.size(); i++){
        gps_edges[i]->computeError();
        avg_error=avg_error+sqrt(gps_edges[i]->chi2())/gps_edges.size();
    }
    std::cout<<"gps edge err after: "<<avg_error<<std::endl;
    avg_error=0;
    for(int i=0; i<sim3_edge_list.size(); i++){
        sim3_edge_list[i]->computeError();
        avg_error=avg_error+sqrt(sim3_edge_list[i]->chi2())/sim3_edge_list.size();
        if(avg_error<0){
            return;
        }
        
    }
    std::cout<<"sim3 edge err after: "<<avg_error<<std::endl;
    for(int i=0; i<sim3_list.size(); i++){
        g2o::Sim3 CorrectedSiw =  v_sim3_list[i]->estimate();
        Eigen::Matrix3d eigR = CorrectedSiw.rotation().toRotationMatrix();
        Eigen::Vector3d eigt = CorrectedSiw.translation();
        double s = CorrectedSiw.scale();
        Eigen::Matrix4d Tiw=Eigen::Matrix4d::Identity();
        eigt *=(1./s); //[R t/s;0 1]
        Tiw.block(0,0,3,3)=eigR;
        Tiw.block(0,3,3,1)=eigt;
        vertex_to_frame[v_sim3_list[i]]->setPose(Tiw.inverse());
    }
}