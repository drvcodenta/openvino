// Copyright (C) 2018-2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "openvino/core/type/element_type.hpp"
#include "openvino/runtime/make_tensor.hpp"
#include "intel_gpu/plugin/remote_context.hpp"
#include "intel_gpu/plugin/common_utils.hpp"
#include "intel_gpu/plugin/remote_tensor.hpp"
#include "intel_gpu/plugin/variable_state.hpp"
#include "intel_gpu/runtime/memory_caps.hpp"
#include "intel_gpu/runtime/layout.hpp"
#include "intel_gpu/runtime/debug_configuration.hpp"

#include <memory>

namespace ov {
namespace intel_gpu {

VariableState::VariableState(const VariableStateInfo& info, RemoteContextImpl::Ptr context, std::shared_ptr<cldnn::ShapePredictor> shape_predictor)
    : ov::IVariableState {info.m_id}
    , m_layout(info.m_layout)
    , m_user_specified_type(info.m_user_specified_type)
    , m_context(context)
    , m_shape_predictor(shape_predictor) {
    update_device_buffer();
}

void VariableState::reset() {
    m_is_set = false;
}

cldnn::memory::ptr VariableState::get_memory() const {
    return m_memory;
}

const cldnn::layout& VariableState::get_layout() const {
    return m_layout;
}

bool VariableState::is_set() const {
    return m_is_set;
}
void VariableState::set() {
    m_is_set = true;
}

void VariableState::set_layout(const cldnn::layout& new_layout) {
    m_layout = new_layout;
    GPU_DEBUG_TRACE_DETAIL << "Update state layout to " << new_layout.to_short_string() << std::endl;
    update_device_buffer();
}

void VariableState::set_state(const ov::SoPtr<ov::ITensor>& state) {
    const bool blocking = true;
    auto remote_ptr = std::dynamic_pointer_cast<RemoteTensorImpl>(state._ptr);
    m_layout.set_partial_shape(state->get_shape());
    update_device_buffer();
    if (remote_ptr != nullptr) {
        auto user_memory = remote_ptr->get_memory();
        m_memory->copy_from(m_context->get_engine().get_service_stream(), *user_memory, blocking);
    } else {
        auto data = state->data();
        m_memory->copy_from(m_context->get_engine().get_service_stream(), data, blocking);
    }
    set();
}

void VariableState::update_device_buffer() {
    if (m_layout.is_dynamic() || m_layout.bytes_count() == 0)
        return;

    if (actual_size < m_layout.bytes_count()) {
        const auto alloc_type = m_context->get_engine().use_unified_shared_memory() ? cldnn::allocation_type::usm_device : cldnn::allocation_type::cl_mem;
        const auto current_shape = get_tensor_shape(m_layout.get_partial_shape());
        const auto alloc_shape = predict_shape(m_name, current_shape, m_layout.data_type, *m_shape_predictor);
        const auto alloc_layout = cldnn::layout(alloc_shape, m_layout.data_type, m_layout.format);
        m_memory = m_context->get_engine().allocate_memory(alloc_layout, alloc_type, false);
        actual_size = std::max(actual_size, alloc_layout.bytes_count());
    }
    m_memory = m_context->get_engine().reinterpret_buffer(*m_memory, m_layout);
}

ov::element::Type VariableState::get_user_specified_type() const {
    return m_user_specified_type != ov::element::undefined ? m_user_specified_type : ov::element::Type(m_layout.data_type);
}

ov::SoPtr<ov::ITensor> VariableState::get_state() const {
    auto tensor = m_context->create_host_tensor(get_user_specified_type(), m_memory->get_layout().get_shape());

    convert_and_copy(m_memory, tensor._ptr.get(), m_context->get_engine().get_service_stream());

    return tensor;
}

}  // namespace intel_gpu
}  // namespace ov
