////////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2014-2016, Lawrence Livermore National Security, LLC.
// Produced at the Lawrence Livermore National Laboratory.
// Written by the LBANN Research Team (B. Van Essen, et al.) listed in
// the CONTRIBUTORS file. <lbann-dev@llnl.gov>
//
// LLNL-CODE-697807.
// All rights reserved.
//
// This file is part of LBANN: Livermore Big Artificial Neural Network
// Toolkit. For details, see http://software.llnl.gov/LBANN or
// https://github.com/LLNL/LBANN.
//
// Licensed under the Apache License, Version 2.0 (the "Licensee"); you
// may not use this file except in compliance with the License.  You may
// obtain a copy of the License at:
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the license.
//
// lbann_layer_softmax .hpp .cpp - softmax layer
////////////////////////////////////////////////////////////////////////////////

#ifndef LBANN_LAYER_SOFTMAX_HPP_INCLUDED
#define LBANN_LAYER_SOFTMAX_HPP_INCLUDED

#include "lbann/layers/lbann_layer.hpp"
#include "lbann/lbann_Elemental_extensions.h"
#include "lbann/io/lbann_file_io.hpp"
#include "lbann/utils/lbann_random.hpp"
#include "lbann/models/lbann_model.hpp"
#include <unistd.h>
#include <string>

namespace lbann {
template <class T_layout>
class softmax_layer: public activation_layer<T_layout> {

 private:
  AbsDistMat *m_workspace;
  AbsDistMat *m_workspace_v;

 public:
  softmax_layer(T_layout data_dist,
               const uint index,
               const int numPrevNeurons,
               const uint numNeurons,
               const uint mini_batch_size,
               const weight_initialization init,
               lbann_comm *comm,
               optimizer *opt)
    :  activation_layer<T_layout>(data_dist, index, comm, mini_batch_size,
                                  numNeurons) {
    this->m_type = layer_type::softmax;
    this->m_index = index;

    // Setup the data distribution
    switch(data_dist) {
    case data_layout::MODEL_PARALLEL:
      initialize_model_parallel_distribution();
      break;
    case data_layout::DATA_PARALLEL:
      initialize_data_parallel_distribution();
      break;
    default:
      throw lbann_exception(std::string{} + __FILE__ + " " +
                            std::to_string(__LINE__) +
                            "Invalid data layout selected");
    }
  }

  ~softmax_layer(void) {
    delete m_workspace;
    delete m_workspace_v;
  }

  /// Matrices should be in MC,MR distributions
  virtual void initialize_model_parallel_distribution() {
    m_workspace = new StarMRMat(this->m_comm->get_model_grid());
    m_workspace_v = new StarMRMat(this->m_comm->get_model_grid());
  }

  /// Weight matrices should be in Star,Star and data matrices Star,VC distributions
  virtual void initialize_data_parallel_distribution(void) {
    m_workspace = new StarVCMat(this->m_comm->get_model_grid());
    m_workspace_v = new StarVCMat(this->m_comm->get_model_grid());
  }

  void setup(int numPrevNeurons) {
    Layer::setup(numPrevNeurons);

    // Initialize other matrices
    Zeros(*this->m_prev_error_signal, this->m_num_neurons, this->m_mini_batch_size);
    Zeros(*this->m_error_signal, numPrevNeurons, this->m_mini_batch_size); // m_error_signal holds the product of m_weights^T * m_prev_error_signal
    Zeros(*this->m_weighted_sum, this->m_num_neurons, this->m_mini_batch_size);
    Zeros(*this->m_activations, this->m_num_neurons, this->m_mini_batch_size);
    Zeros(*this->m_prev_activations, numPrevNeurons, this->m_mini_batch_size);
    Zeros(*this->m_workspace, 1, this->m_mini_batch_size);
  }

  void fp_set_std_matrix_view(void) {
    int64_t cur_mini_batch_size = this->m_neural_network_model->get_current_mini_batch_size();
    Layer::fp_set_std_matrix_view();
    View(*m_workspace_v, *m_workspace, ALL, IR(0, cur_mini_batch_size));
  }

  void fp_compute() {

    // Get local matrices and parameters
    Mat& workspace_local = m_workspace_v->Matrix();
    Mat& activations_local = this->m_activations_v->Matrix();
    const Int local_height = activations_local.Height();
    const Int local_width = activations_local.Width();

    // Find maximum entry in each column
#pragma omp parallel for
    for(Int c=0; c<local_width; ++c) {
    DataType max_entry = -INFINITY;
    for(Int r=0; r<local_height; ++r) {
    max_entry = Max(max_entry, activations_local.Get(r,c));
  }
    workspace_local.Set(Int(0), c, max_entry);
  }
    AllReduce(*m_workspace_v, m_workspace_v->RedundantComm(), mpi::MAX);

    // Subtract column max and exponentiate activations
    // Note: Subtracting the column max prevents activations from blowing
    //   up. Large negative values underflow to 0.
    IndexDependentMap(activations_local,
      (std::function<DataType(Int,Int,const DataType&)>)
      ([this,&workspace_local](Int r, Int c, const DataType& z)->DataType {
    return Exp(z - workspace_local.Get(Int(0), c));
  }));

    // Compute column sums
#pragma omp parallel for
    for(Int c=0; c<local_width; ++c) {
      DataType sum = 0;
      for(Int r=0; r<local_height; ++r) {
        sum += activations_local.Get(r,c);
      }
      workspace_local.Set(Int(0), c, sum);
    }
    AllReduce(*m_workspace_v, m_workspace_v->RedundantComm(), mpi::SUM);

    // Divide activations by column sums
    // This truncates small values to 0 to avoid them becoming denormalized later
    // in the forward/backward stages. Denormalized values can significantly
    // impact floating point performance.
    IndexDependentMap(activations_local,
                      (std::function<DataType(Int,Int,const DataType&)>)
                      ([this,&workspace_local](Int r, Int c, const DataType& z)->DataType {
                        const DataType v = z / workspace_local.Get(Int(0), c);
                        return Abs(v) < 1e-8 ? DataType(1e-8) : v;
                      }));

  }

  void bp_compute(void) {

    // Stop early if objective function is categorical cross entropy
    // Note: error signal is already computed in objective function object
    if(this->m_neural_network_model->m_obj_fn->type == objective_functions::obj_fn_type::categorical_cross_entropy
       && (this->m_next_layer_type == layer_type::target_distributed_minibatch
           || this->m_next_layer_type == layer_type::target_distributed_minibatch_parallel_io
           || this->m_next_layer_type == layer_type::target_partitioned_minibatch_parallel_io
           // || m_next_layer_type == layer_type::target_unsupervised
           )) {
      return;
    }

    // Get local matrices and parameters
    const Mat& activations_local = this->m_activations_v->LockedMatrix();
    Mat& workspace_local = m_workspace_v->Matrix();
    Mat& prev_error_signal_local = this->m_prev_error_signal_v->Matrix();
    //const Int local_height = activations_local.Height();
    const Int local_width = activations_local.Width();

    // Compute dot products
    // Note: prev_error_signal^T activations
    for(Int c=0; c<local_width; ++c) {
      workspace_local.Set(Int(0), c,
                          Dot(prev_error_signal_local(ALL,IR(c)),
                              activations_local(ALL,IR(c))));
    }
    AllReduce(*m_workspace_v, m_workspace_v->RedundantComm(), mpi::SUM);

    // Update error signal
    // Note: prev_error_signal := activations * (prev_error_signal - prev_error_signal^T activations)
    IndexDependentMap(prev_error_signal_local,
                      (std::function<DataType(Int,Int,const DataType&)>)
                      ([this,&activations_local,&workspace_local]
                       (Int r, Int c, const DataType& z)->DataType {
                        const DataType activations_entry = activations_local.Get(r,c);
                        const DataType dot_product_entry = workspace_local.Get(Int(0),c);
                        return activations_entry * (z - dot_product_entry);
                      }));

  }

  bool update_compute(void) {
    if(this->m_execution_mode == execution_mode::training) {
      double start = get_time();
      this->update_time += get_time() - start;
    }
    return true;
  }

  bool saveToCheckpoint(int fd, const char *filename, uint64_t *bytes) {
    return Layer::saveToCheckpoint(fd, filename, bytes);
  }

  bool loadFromCheckpoint(int fd, const char *filename, uint64_t *bytes) {
    return Layer::loadFromCheckpoint(fd, filename, bytes);
  }

  bool saveToCheckpointShared(lbann::persist& p) {
    return Layer::saveToCheckpointShared(p);
  }

  bool loadFromCheckpointShared(lbann::persist& p) {
    return Layer::loadFromCheckpointShared(p);
  }
};
}

#endif // LBANN_LAYER_SOFTMAX_HPP_INCLUDED
