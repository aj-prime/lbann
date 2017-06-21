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
// lbann_dropout .hpp - SELU dropout
////////////////////////////////////////////////////////////////////////////////

#ifndef LBANN_LAYER_REGULARIZER_SELU_DROPOUT_HPP_INCLUDED
#define LBANN_LAYER_REGULARIZER_SELU_DROPOUT_HPP_INCLUDED

#include "lbann/layers/regularizers/regularizer.hpp"

namespace lbann {

/**
 * SELU dropout: alpha-scaled dropout for use with SELU activations.
 * See: Klambauer et al. "Self-Normalizing Neural Networks", 2017.
 * This makes the same default assumptions as our SELU activations.
 * The paper recommends a default dropout rate of 0.05 (keep 0.95).
 */
template <data_layout T_layout>
class selu_dropout : public regularizer_layer {
 public:
  /** Keep units with probabiliy keep_prob. */
  selu_dropout(const uint index, const uint num_neurons, lbann_comm *comm,
               uint mini_batch_size, float keep_prob=0.95f,
               DataType alpha = DataType(1.6732632423543772848170429916717),
               DataType scale = DataType(1.0507009873554804934193349852946)) :
    regularizer_layer(index, comm, mini_batch_size),
    m_keep_prob(keep_prob) {
#ifdef LBANN_PROCDET_DROPOUT
    throw lbann_exception("selu_dropout: deterministic dropout not supported");
#endif
    // Setup the data distribution
    initialize_distributed_matrices();
    this->m_type = layer_type::dropout;
    this->m_num_neurons = num_neurons;
    // Compute alpha' and the affine transform.
    m_alpha_prime = -scale*alpha;
    m_a = keep_prob +
      m_alpha_prime*m_alpha_prime*keep_prob*(DataType(1) - keep_prob);
    m_a = DataType(1) / std::sqrt(m_a);
    m_b = -m_a * m_alpha_prime*(DataType(1) - keep_prob);
  }

  ~selu_dropout() {
    delete m_cur_mask;
  }

  virtual inline void initialize_distributed_matrices();
  virtual inline data_layout get_data_layout() { return T_layout; }

  void setup(int num_prev_neurons) {
    regularizer_layer::setup(num_prev_neurons);
    this->m_num_neurons = num_prev_neurons;
    Zeros(*(this->m_activations), this->m_num_neurons, this->m_mini_batch_size);
    Zeros(*(this->m_error_signal), num_prev_neurons, this->m_mini_batch_size);
  }

 protected:
  /** Drop out units in forward propagation. */
  void fp_compute() {
    if (this->get_execution_mode() != execution_mode::training ||
        m_keep_prob < 0.0f) {
      // Copy previous activations over.
      El::Copy(*(this->m_prev_activations), *(this->m_activations));
      return;
    }
    ElMat *input_acts = this->m_prev_activations;
    const El::Int local_height = input_acts->LocalHeight();
    const El::Int local_width = input_acts->LocalWidth();

    Mat& local_input_acts = input_acts->Matrix();
    Mat& local_output_acts = this->m_activations->Matrix();

    // Construct and apply mask and the affine transform.
    // TODO: Optimize.
    El::Bernoulli(*m_cur_mask, local_height, local_width, m_keep_prob);
    for (El::Int col = 0; col < local_width; ++col) {
      for (El::Int row = 0; row < local_height; ++row) {
        local_output_acts(row, col) = m_a *
          (local_input_acts(row, col)*(*m_cur_mask)(row, col) +
           m_alpha_prime*(1 - (*m_cur_mask)(row, col))) + m_b;
      }
    }
  }

  /** Adjust gradients for dropout in backprop. */
  void bp_compute() {
    // Terminate early when not training.
    if (this->get_execution_mode() != execution_mode::training) {
      return;
    }
    if (m_keep_prob < 0.0f) {
      // Copy error signal through.
      El::Copy(*(this->m_prev_error_signal), *(this->m_error_signal));
      return;
    }

    Mat& local_prev_error_signal = this->m_prev_error_signal->Matrix();
    Mat& local_error_signal = this->m_error_signal->Matrix();
    const El::Int local_height = local_prev_error_signal.Height();
    const El::Int local_width = local_prev_error_signal.Width();
    // Reweight with the affine scale factor and the dropout mask.
    for (El::Int col = 0; col < local_width; ++col) {
      for (El::Int row = 0; row < local_height; ++row) {
        local_error_signal(row, col) =
          m_a * local_prev_error_signal(row, col) * (*m_cur_mask)(row, col);
      }
    }
  }

 private:
  /** Alpha prime, the low-variance saturation point. */
  DataType m_alpha_prime;
  /** Affine scaling parameter to keep mean/variance at desired value. */
  DataType m_a;
  /** Affine additive parameter to keep mean/variance at desired value. */
  DataType m_b;
  /** Probability of keeping each unit. */
  float m_keep_prob;
  /** Current dropout mask (a scaled Bernoulli random matrix). */
  Mat *m_cur_mask;
};

template<> inline void selu_dropout<data_layout::MODEL_PARALLEL>::initialize_distributed_matrices() {
  regularizer_layer::initialize_distributed_matrices<data_layout::MODEL_PARALLEL>();
  m_cur_mask = new Mat;
}

template<> inline void selu_dropout<data_layout::DATA_PARALLEL>::initialize_distributed_matrices() {
  regularizer_layer::initialize_distributed_matrices<data_layout::DATA_PARALLEL>();
  m_cur_mask = new Mat;
}

}  // namespace lbann

#endif  // LBANN_LAYER_REGULARIZER_SELU_DROPOUT_HPP_INCLUDED