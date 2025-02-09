#include <filesystem>
#include <iostream>
#include <vector>
#include <string>
#include <fstream>

#include "mujoco/mujoco.h"
#include "Eigen/Dense"

#include "src/unitree_go2/operational_space_controller.h"

#include "src/unitree_go2/autogen/autogen_functions.h"
#include "src/unitree_go2/autogen/autogen_defines.h"


int main(int argc, char** argv){
    // Real Data:
    OperationalSpaceController osc;
    std::filesystem::path xml_path = "models/unitree_go2/scene_mjx_torque.xml";
    osc.initialize(xml_path);

    Eigen::VectorXd q_init =  Eigen::Map<Eigen::VectorXd>(osc.mj_model->key_qpos, constants::model::nq_size);
    Eigen::VectorXd qd_init =  Eigen::Map<Eigen::VectorXd>(osc.mj_model->key_qvel, constants::model::nv_size);
    Eigen::VectorXd ctrl =  Eigen::Map<Eigen::VectorXd>(osc.mj_model->key_ctrl, constants::model::nu_size);

    // Set initial state:
    osc.mj_data->qpos = q_init.data();
    osc.mj_data->qvel = qd_init.data();
    osc.mj_data->ctrl = ctrl.data();

    // Desired Motor States:
    Eigen::VectorXd q_desired(osc.mj_model->nu);
    q_desired << 0, 0.9, -1.8, 0, 0.9, -1.8, 0, 0.9, -1.8, 0, 0.9, -1.8;
    Eigen::VectorXd qd_desired = Eigen::VectorXd::Zero(q_desired.size());

    mj_forward(osc.mj_model, osc.mj_data);

    Eigen::Matrix<double, constants::model::site_ids_size, 3, Eigen::RowMajor> points = 
        Eigen::Matrix<double, constants::model::site_ids_size, 3, Eigen::RowMajor>::Zero();
    points = Eigen::Map<Eigen::Matrix<double, constants::model::site_ids_size, 3, Eigen::RowMajor>>(
        osc.mj_data->site_xpos
    );
    OSCData osc_data = osc.get_data(points);

    Eigen::VectorXd design_vector = Eigen::VectorXd::Zero(constants::optimization::design_vector_size);
    Eigen::MatrixXd M = Eigen::Map<Eigen::MatrixXd>(osc_data.mass_matrix.data(), constants::model::nv_size, constants::model::nv_size);
    Eigen::VectorXd C = osc_data.coriolis_matrix;
    Eigen::MatrixXd Jc = Eigen::Map<Eigen::MatrixXd>(osc_data.contact_jacobian.data(), constants::model::nv_size, constants::optimization::z_size);
    Eigen::MatrixXd J = Eigen::Map<Eigen::MatrixXd>(osc_data.taskspace_jacobian.data(), 6 * constants::model::body_ids_size, constants::model::nv_size);
    Eigen::VectorXd b = osc_data.taskspace_bias;

    Eigen::MatrixXd ddx_desired = Eigen::MatrixXd::Zero(constants::model::site_ids_size, 6);

    // Save matrix to CSV:
    {
        std::ofstream file("M.csv");
        if (file.is_open()) {
            file << M.format(Eigen::IOFormat(Eigen::FullPrecision, Eigen::DontAlignCols, ", ", "\n"));
            file.close();
        }
    }

    // Save C vector to CSV:
    {
        std::ofstream file("C.csv");
        if (file.is_open()) {
            file << C.format(Eigen::IOFormat(Eigen::FullPrecision, Eigen::DontAlignCols, ", ", "\n"));
            file.close();
        }
    }

    // Save Jc matrix to CSV:
    {
        std::ofstream file("Jc.csv");
        if (file.is_open()) {
            file << Jc.format(Eigen::IOFormat(Eigen::FullPrecision, Eigen::DontAlignCols, ", ", "\n"));
            file.close();
        }
    }

    // Save J matrix to CSV:
    {
        std::ofstream file("J.csv");
        if (file.is_open()) {
            file << J.format(Eigen::IOFormat(Eigen::FullPrecision, Eigen::DontAlignCols, ", ", "\n"));
            file.close();
        }
    }

    // Save b vector to CSV:
    {
        std::ofstream file("b.csv");
        if (file.is_open()) {
            file << b.format(Eigen::IOFormat(Eigen::FullPrecision, Eigen::DontAlignCols, ", ", "\n"));
            file.close();
        }
    }

    // Save ddx_desired matrix to CSV:
    {
        std::ofstream file("ddx_desired.csv");
        if (file.is_open()) {
            file << ddx_desired.format(Eigen::IOFormat(Eigen::FullPrecision, Eigen::DontAlignCols, ", ", "\n"));
            file.close();
        }
    }

    // Evaluate the Aeq Function:
    {
        // Allocate Result Object:
        double res0[constants::optimization::Aeq_sz];

        // Casadi Work Vector and Memory:
        const double *args[Aeq_SZ_ARG];
        double *res[Aeq_SZ_RES];
        casadi_int iw[Aeq_SZ_IW];
        double w[Aeq_SZ_W];

        res[0] = res0;

        Aeq_incref();

        // Copy the C Arrays:
        args[0] = design_vector.data();
        args[1] = M.data();
        args[2] = C.data();
        args[3] = Jc.data();

        // Initialize Memory:
        int mem = Aeq_alloc_mem();
        Aeq_init_mem(mem);

        // Evaluate the Function:
        Aeq(args, res, iw, w, mem);
        
        // Map the Result to Eigen Matrix:
        Eigen::MatrixXd Aeq = Eigen::Map<Eigen::MatrixXd>(res0, constants::optimization::Aeq_rows, constants::optimization::Aeq_cols);

        {
            std::ofstream file("Aeq.csv");
            if (file.is_open()) {
                file << Aeq.format(Eigen::IOFormat(Eigen::FullPrecision, Eigen::DontAlignCols, ", ", "\n"));
                file.close();
            }
        }

        // Free Memory:
        Aeq_free_mem(mem);
        Aeq_decref();
    }

    // Evaluate the beq function:
    {
        // Allocate Result Object:
        double res0[constants::optimization::beq_sz];

        // Casadi Work Vector and Memory:
        const double *args[beq_SZ_ARG];
        double *res[beq_SZ_RES];
        casadi_int iw[beq_SZ_IW];
        double w[beq_SZ_W];

        res[0] = res0;

        beq_incref();
        
        // Copy the C Arrays:
        args[0] = design_vector.data();
        args[1] = M.data();
        args[2] = C.data();
        args[3] = Jc.data();

        // Initialize Memory:
        int mem = beq_alloc_mem();
        beq_init_mem(mem);

        // Evaluate the Function:
        beq(args, res, iw, w, mem);

        // Map the Result to Eigen Matrix:
        Eigen::VectorXd beq = Eigen::Map<Eigen::VectorXd>(res0, constants::optimization::beq_sz);

        {
            std::ofstream file("beq.csv");
            if (file.is_open()) {
                file << beq.format(Eigen::IOFormat(Eigen::FullPrecision, Eigen::DontAlignCols, ", ", "\n"));
                file.close();
            }
        }

        // Free Memory:
        beq_free_mem(mem);
        beq_decref();
    }

    // Evaluate the Aineq Function:
    {
        // Allocate Result Object:
        double res0[constants::optimization::Aineq_sz];

        // Casadi Work Vector and Memory:
        const double *args[Aineq_SZ_ARG];
        double *res[Aineq_SZ_RES];
        casadi_int iw[Aineq_SZ_IW];
        double w[Aineq_SZ_W];

        res[0] = res0;

        Aineq_incref();

        // Copy the C Arrays:
        args[0] = design_vector.data();

        // Initialize Memory:
        int mem = Aineq_alloc_mem();
        Aineq_init_mem(mem);

        // Evaluate the Function:
        Aineq(args, res, iw, w, mem);
        
        // Map the Result to Eigen Matrix:
        Eigen::MatrixXd Aineq = Eigen::Map<Eigen::MatrixXd>(res0, constants::optimization::Aineq_rows, constants::optimization::Aineq_cols);

        {
            std::ofstream file("Aineq.csv");
            if (file.is_open()) {
                file << Aineq.format(Eigen::IOFormat(Eigen::FullPrecision, Eigen::DontAlignCols, ", ", "\n"));
                file.close();
            }
        }

        // Free Memory:
        Aineq_free_mem(mem);
        Aineq_decref();
    }

    // Evaluate the bineq function:
    {
        // Allocate Result Object:
        double res0[constants::optimization::bineq_sz];

        // Casadi Work Vector and Memory:
        const double *args[bineq_SZ_ARG];
        double *res[bineq_SZ_RES];
        casadi_int iw[bineq_SZ_IW];
        double w[bineq_SZ_W];

        res[0] = res0;

        bineq_incref();
        
        // Copy the C Arrays:
        args[0] = design_vector.data();

        // Initialize Memory:
        int mem = bineq_alloc_mem();
        bineq_init_mem(mem);

        // Evaluate the Function:
        bineq(args, res, iw, w, mem);

        // Map the Result to Eigen Matrix:
        Eigen::VectorXd bineq = Eigen::Map<Eigen::VectorXd>(res0, constants::optimization::bineq_sz);

        {
            std::ofstream file("bineq.csv");
            if (file.is_open()) {
                file << bineq.format(Eigen::IOFormat(Eigen::FullPrecision, Eigen::DontAlignCols, ", ", "\n"));
                file.close();
            }
        }

        // Free Memory:
        bineq_free_mem(mem);
        bineq_decref();
    }

    // Evaluate the H function:
    {
        // Allocate Result Object:
        double res0[constants::optimization::H_sz];

        // Casadi Work Vector and Memory:
        const double *args[H_SZ_ARG];
        double *res[H_SZ_RES];
        casadi_int iw[H_SZ_IW];
        double w[H_SZ_W];

        res[0] = res0;

        H_incref();
        
        // Copy the C Arrays:
        args[0] = design_vector.data();
        args[1] = ddx_desired.data();
        args[2] = J.data();
        args[3] = b.data();
        
        // Initialize Memory:
        int mem = H_alloc_mem();
        H_init_mem(mem);

        // Evaluate the Function:
        H(args, res, iw, w, mem);

        // Map the Result to Eigen Matrix:
        Eigen::MatrixXd H = Eigen::Map<Eigen::MatrixXd>(res0, constants::optimization::H_rows, constants::optimization::H_cols);

        {
            std::ofstream file("H.csv");
            if (file.is_open()) {
                file << H.format(Eigen::IOFormat(Eigen::FullPrecision, Eigen::DontAlignCols, ", ", "\n"));
                file.close();
            }
        }

        // Free Memory:
        H_free_mem(mem);
        H_decref();
    }

    // Evaluate the f function:
    {
        // Allocate Result Object:
        double res0[constants::optimization::f_sz];

        // Casadi Work Vector and Memory:
        const double *args[f_SZ_ARG];
        double *res[f_SZ_RES];
        casadi_int iw[f_SZ_IW];
        double w[f_SZ_W];

        res[0] = res0;

        f_incref();
        
        // Copy the C Arrays:
        args[0] = design_vector.data();
        args[1] = ddx_desired.data();
        args[2] = J.data();
        args[3] = b.data();
        
        // Initialize Memory:
        int mem = f_alloc_mem();
        f_init_mem(mem);

        // Evaluate the Function:
        f(args, res, iw, w, mem);

        // Map the Result to Eigen Matrix:
        Eigen::VectorXd f = Eigen::Map<Eigen::VectorXd>(res0, constants::optimization::f_sz);

        {
            std::ofstream file("f.csv");
            if (file.is_open()) {
                file << f.format(Eigen::IOFormat(Eigen::FullPrecision, Eigen::DontAlignCols, ", ", "\n"));
                file.close();
            }
        }

        // Free Memory:
        f_free_mem(mem);
        f_decref();
    }

    return 0;
}

