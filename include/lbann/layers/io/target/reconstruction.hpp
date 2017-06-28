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
////////////////////////////////////////////////////////////////////////////////

#ifndef LBANN_LAYERS_RECONSTRUCTION_HPP_INCLUDED
#define LBANN_LAYERS_RECONSTRUCTION_HPP_INCLUDED

#include "lbann/layers/lbann_layer.hpp"
#include "lbann/layers/io/target/lbann_target_layer.hpp"
#include "lbann/models/lbann_model.hpp"
#include <string>
#include "lbann/utils/lbann_random.hpp"

namespace lbann {
template <data_layout T_layout>
class reconstruction_layer : public target_layer {
 private:
  Layer *m_original_layer;
  DataType aggregate_cost;
  long num_forwardprop_steps;
  //  weight_initialization m_weight_initialization;
  DistMat original_layer_act_v;

 public:
  /// @todo note that the reconstruction layer used to use weight_initialization::glorot_uniform
  reconstruction_layer(int index,
                       lbann_comm *comm,
                       optimizer *opt,/*needed?*/
                       int minim_batch_size,
                       Layer *original_layer)
    :  target_layer(comm, minim_batch_size, {}, false), m_original_layer(original_layer) {
    // Setup the data distribution
    initialize_distributed_matrices();
    this->m_index = index;
    aggregate_cost = 0.0;
    num_forwardprop_steps = 0;
    this->m_num_neurons = m_original_layer->get_num_neurons();
    this->m_neuron_dims = m_original_layer->get_neuron_dims();
    this->m_num_neuron_dims = m_original_layer->get_num_neuron_dims();
  }

  std::string get_name() const { return "reconstruction"; }

  virtual inline void initialize_distributed_matrices() {
    target_layer::initialize_distributed_matrices<T_layout>();
  }
  virtual data_layout get_data_layout() const { return T_layout; }

  virtual void setup(Layer *prev_layer, Layer *next_layer) {
    target_layer::setup(prev_layer, next_layer);

    // Initialize other matrices
    Zeros(*this->m_error_signal, this->m_num_prev_neurons, this->m_mini_batch_size); // m_error_signal holds the product of m_weights^T * m_prev_error_signal
    Zeros(*this->m_activations, this->m_num_neurons, this->m_mini_batch_size); //clear up m_activations before copying fp_input to it
    Zeros(*this->m_prev_error_signal, this->m_num_neurons, this->m_mini_batch_size); //clear up before filling with new results
    Zeros(*this->m_prev_activations, this->m_num_prev_neurons, this->m_mini_batch_size);
  }

 protected:
  void fp_set_std_matrix_view() {
    int64_t cur_mini_batch_size = this->m_neural_network_model->get_current_mini_batch_size();

    target_layer::fp_set_std_matrix_view();

    //view of original layer
    View(original_layer_act_v,*(m_original_layer->m_activations),IR(0,m_original_layer->m_activations->Height()),IR(0,cur_mini_batch_size));
  }


  void fp_compute() {
    // Compute cost will be sum of squared error of fp_input (linearly transformed to m_activations)
    // and original layer fp_input/original input
    DataType avg_error = this->m_neural_network_model->m_obj_fn->compute_obj_fn(*this->m_prev_activations_v, original_layer_act_v);
    aggregate_cost += avg_error;
    num_forwardprop_steps++;
  }

  void bp_compute() {
    // Compute error signal
    this->m_neural_network_model->m_obj_fn->compute_obj_fn_derivative(m_prev_layer, *this->m_prev_activations_v, original_layer_act_v,*this->m_error_signal_v);

    //m_prev_error_signal_v is the error computed by objective function
    //is really not previous, but computed in this layer
    //@todo: rename as obj_error_signal
  }

 public:
  execution_mode get_execution_mode() {
    return this->m_execution_mode;
  }

  bool update_compute() {
    if(this->m_execution_mode == execution_mode::training) {
      double start = get_time();
      this->update_time += get_time() - start;
    }
    return true;
  }

  void summarize(lbann_summary& summarizer, int64_t step) {
    Layer::summarize(summarizer, step);
    std::string tag = "layer" + std::to_string(static_cast<long long>(this->m_index))
      + "/ReconstructionCost";
    summarizer.reduce_scalar(tag, average_cost(), step);
  }

  void epoch_print() const {
    double avg_cost = average_cost();
    if (this->m_comm->am_world_master()) {
      std::vector<double> avg_costs(this->m_comm->get_num_models());
      this->m_comm->intermodel_gather(avg_cost, avg_costs);
      for (size_t i = 0; i < avg_costs.size(); ++i) {
        std::cout << "model " << i << " average reconstruction cost: " << avg_costs[i] << std::endl;
      }
    } else {
      this->m_comm->intermodel_gather(avg_cost, this->m_comm->get_world_master());
    }
  }

  void epoch_reset() {
    Layer::epoch_reset();
    reset_cost();
  }

  void reset_cost() {
    aggregate_cost = 0.0;
    num_forwardprop_steps = 0;
  }

  DataType average_cost() const {
    return aggregate_cost / num_forwardprop_steps;
  }

};
}

#endif  // LBANN_LAYERS_RECONSTRUCTION_HPP_INCLUDED
