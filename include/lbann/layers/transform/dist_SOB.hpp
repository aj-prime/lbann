////////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2014-2019, Lawrence Livermore National Security, LLC.
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

#ifndef LBANN_LAYER_DIST_SOB_HPP_INCLUDED
#define LBANN_LAYER_DIST_SOB_HPP_INCLUDED

#include "lbann/layers/data_type_layer.hpp"
#include "lbann/utils/exception.hpp"

namespace lbann {

template <typename TensorDataType,
          data_layout T_layout = data_layout::DATA_PARALLEL,
          El::Device Dev = El::Device::CPU>
class dist_SOB_layer : public data_type_layer<TensorDataType>
{
public:
  dist_SOB_layer(lbann_comm* comm) : data_type_layer<TensorDataType>(comm)
  {
    this->m_expected_num_parent_layers =
      1; // Only one parent but sum will be over branches
  }

  dist_SOB_layer* copy() const override { return new dist_SOB_layer(*this); }
  std::string get_type() const override { return "dist_SOB"; }
  data_layout get_data_layout() const override { return T_layout; }
  El::Device get_device_allocation() const override { return Dev; }

protected:
  El::SyncInfo<Dev> syncSubGridCommunication = El::SyncInfo<Dev>();

  void setup_pointers() override
  {
    data_type_layer<TensorDataType>::setup_pointers();
    if (this->get_num_parents() < 1) {
      std::stringstream err;
      err << get_type() << " layer \"" << this->get_name() << "\" "
          << "has no parent layers";
      LBANN_ERROR(err.str());
    }
  }

  void setup_dims(DataReaderMetaData& dr_metadata) override
  {
    data_type_layer<TensorDataType>::setup_dims(dr_metadata);
    this->set_output_dims(this->get_input_dims());

    // Check that input dimensions match
    const auto& output_dims = this->get_output_dims();
    for (int i = 0; i < this->get_num_parents(); ++i) {
      if (this->get_input_dims(i) != output_dims) {
        const auto& parents = this->get_parent_layers();
        std::stringstream err;
        err << get_type() << " layer \"" << this->get_name() << "\" "
            << "has input tensors with incompatible dimensions (";
        for (int j = 0; j < this->get_num_parents(); ++j) {
          const auto& dims = this->get_input_dims(j);
          err << (j > 0 ? ", " : "") << "layer \"" << parents[j]->get_name()
              << "\" outputs ";
          for (size_t k = 0; k < dims.size(); ++k) {
            err << (k > 0 ? " x " : "") << dims[k];
          }
        }
        err << ")";
        LBANN_ERROR(err.str());
      }
    }
  }

  void fp_compute() override
  {

    auto& output = this->get_activations();
    auto parents = this->get_parent_layers();

    El::DistMatrix<TensorDataType, El::STAR, El::VC, El::ELEMENT, Dev>*
      output_cast = dynamic_cast<
        El::DistMatrix<TensorDataType, El::STAR, El::VC, El::ELEMENT, Dev>*>(
        &output);

    El::mpi::Comm const& CommA = output_cast->Grid().ViewingComm();
    El::SyncInfo<Dev> syncInfoOutput =
      El::SyncInfoFromMatrix(output_cast->LockedMatrix());

    const El::Int mloc = output_cast->LocalHeight();
    const El::Int nloc = output_cast->LocalWidth();

    El::Matrix<TensorDataType, Dev> temp_output(mloc, nloc);
    El::Copy(this->get_prev_activations(0), output);
    El::Copy(output_cast->LockedMatrix(), temp_output);
    El::mpi::AllReduce(temp_output.Buffer(),
                       output_cast->Buffer(),
                       mloc * nloc,
                       El::mpi::SUM,
                       CommA,
                       syncInfoOutput);
    // El::mpi::AllReduce(input.LockedMatrix(), output_cast->Buffer(),
    // mloc*nloc, El::mpi::SUM, CommA, syncInfoOutput); for (int i = 1; i <
    // this->get_num_parents(); ++i) {
    //   El::Axpy(DataType(1), this->get_prev_activations(i), output);
    // }
  }

  void fp_setup_outputs(El::Int mini_batch_size) override
  {

    if (this->get_num_children() < 1) {
      return;
    }
    // Determine distributed matrix alignment
    const bool align_outputs = this->get_num_parents() > 0;
    const auto& alignment_dist =
      (align_outputs ? this->get_prev_activations().DistData()
                     : this->get_activations().DistData());

    // Initialize output tensors
    for (int i = 0; i < this->get_num_children(); ++i) {

      auto& output = this->get_activations(i);
      output.Empty(false);
      if (align_outputs && this->get_parallel_strategy().enable_subgraph == 0) {
        output.AlignWith(alignment_dist);
      }
      output.Resize(this->get_output_size(i), mini_batch_size);
    }
  }

  void bp_setup_gradient_wrt_inputs(El::Int mini_batch_size) override
  {
    int tag = 0;
    const auto& gradient_wrt_output = this->get_prev_error_signals();

    if (this->is_subgraph_parallelism_enabled() &&
        this->get_parallel_strategy().enable_subgraph == 1) {
      auto subgrid_tags = (*this->parent_tags);

      if (this->get_communication_flag())
      // If vector copy is enable, broadcast the gradients from parent grid to
      // multiple subgrids
      {
        auto const* ptr_gradient =
          dynamic_cast<El::DistMatrix<TensorDataType,
                                      El::STAR,
                                      El::VC,
                                      El::ELEMENT,
                                      Dev> const*>(&gradient_wrt_output);
        El::copy::TranslateBetweenGridsBroadcast<TensorDataType, Dev, Dev>(
          *ptr_gradient,
          this->get_branch_tag_input_vector(),
          this->get_subgrid_comm(),
          syncSubGridCommunication);
      }
      else if (this->get_communication_flag() == 1) {
        auto const* ptr_gradient =
          dynamic_cast<El::DistMatrix<TensorDataType,
                                      El::STAR,
                                      El::VC,
                                      El::ELEMENT,
                                      Dev> const*>(&gradient_wrt_output);
        El::copy::TranslateBetweenGridsBroadcast<TensorDataType, Dev, Dev>(
          *ptr_gradient,
          this->get_branch_tag_input_vector());
      }
      else {
        for (int i = 0; i < this->num_spliting_groups; i++) {

          El::Copy(gradient_wrt_output, this->get_branch_tag_input(i));
        }

      } // end vector copy condition

      for (int i = 0; i < this->get_num_parents(); ++i) {
        tag = subgrid_tags[i];

        El::LockedView(this->get_error_signals(i),
                       this->get_branch_tag_input(tag));
      }
    }
    else {
      for (int i = 0; i < this->get_num_parents(); ++i) {

        El::LockedView(this->get_error_signals(i), gradient_wrt_output);
      }
    }
  }

  void bp_compute() override {}
};

LBANN_DEFINE_LAYER_BUILDER(dist_SOB);

#ifndef LBANN_DIST_SOB_LAYER_INSTANTIATE
#define PROTO_DEVICE(T, Device)                                                \
  extern template class dist_SOB_layer<T, data_layout::DATA_PARALLEL, Device>; \
  extern template class dist_SOB_layer<T, data_layout::MODEL_PARALLEL, Device>

#include "lbann/macros/instantiate_device.hpp"
#undef PROTO_DEVICE

#endif // LBANN_DIST_SOB_LAYER_INSTANTIATE

} // namespace lbann

#endif // LBANN_LAYER_SUM_HPP_INCLUDED
