/**
 * \file src/opr-mm/impl/io_remote.cpp
 * MegEngine is Licensed under the Apache License, Version 2.0 (the "License")
 *
 * Copyright (c) 2014-2020 Megvii Inc. All rights reserved.
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT ARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

#include "megbrain/opr/io_remote.h"
#include "megbrain/comp_node_env.h"
#include "megbrain/graph/grad_impl.h"
#include "megbrain/opr/megray_helper.h"
#include "megbrain/serialization/sereg.h"

using namespace mgb;
using namespace opr;

cudaStream_t get_stream(VarNode* var) {
    return CompNodeEnv::from_comp_node(var->comp_node()).cuda_env().stream;
}

/* ===================== RemoteSend ===================== */

MGB_DYN_TYPE_OBJ_FINAL_IMPL(RemoteSend);

RemoteSend::RemoteSend(const PeerDesc& peer, VarNode* var,
                       std::shared_ptr<GroupClient> group_client,
                       const OperatorNodeConfig& config) :
        Super(var->owner_graph(), config, "remote_send", {var}) {
    m_peer = peer;
    m_group_client = group_client;

    add_input({var});
    auto ovar = add_output(None);
    if (!peer.is_grad) {
        ovar->add_flag(VarNode::Flag::ALLOW_EMPTY_SHAPE)
                .add_flag(VarNode::Flag::VOLATILE_CONTENT);
    }
    add_equivalence_component<ScalarHash<void*>>(this);
}

SymbolVar RemoteSend::make(const PeerDesc& peer, SymbolVar var,
                           std::shared_ptr<GroupClient> group_client,
                           const OperatorNodeConfig& config) {
    return var.insert_single_output_opr<RemoteSend>(peer, var.node(),
                                                    group_client, config);
}

void RemoteSend::scn_do_execute() {
    if (!m_init) {
        auto&& comp_node = output(0)->comp_node();

        // rank 0 for RemoteSend
        auto reg_info = m_group_client->opr_register(m_peer.key, 2, 0, false,
                comp_node.get_uid());

        m_megray_comm = MegRayCommBuilder::get_megray_comm(
                reg_info.hash, m_peer.key, 2, 0, MegRay::MEGRAY_UCX, m_group_client);

        m_megray_ctx = MegRay::CudaContext::make(get_stream(output(0)));

        m_init = true;
    }

    mgb_assert(m_init);
    size_t data_size = 1;
    auto&& tensor = input(0)->dev_tensor();
    auto&& ishp = tensor.shape();
    for (size_t i = 0; i < ishp.ndim; i++) {
        data_size *= ishp[i];
    }
    data_size *= tensor.dtype().size();
    auto status = m_megray_comm->send(tensor.raw_ptr(), data_size, 1, m_megray_ctx);
    mgb_assert(status == MegRay::MEGRAY_OK, "MegRay send failed");

    if (m_peer.is_grad) {
        auto&& dest = output(0)->dev_tensor();
        if (m_output_val.empty()) {
            m_output_val.comp_node(dest.comp_node())
                    .dtype(dest.dtype())
                    .resize({1});
            memset(m_output_val.raw_ptr(), 0, m_output_val.dtype().size());
        }
        dest.copy_from_fixlayout(m_output_val);
    }
}

void RemoteSend::init_output_static_infer_desc() {
    using namespace cg::static_infer;
    auto&& mgr = owner_graph()->static_infer_manager();
    auto do_infer = [this](TensorShape& dest, const InpVal&) {
        if (peer_desc().is_grad) {
            dest = {1};
        } else {
            dest = {0};
        }
        return true;
    };
    mgr.register_shape_infer(output(0), {SourceType::CONSTANT, {}, do_infer});
}

cg::OperatorNodeBase::NodeProp* RemoteSend::do_make_node_prop() const {
    auto prop = RemoteIOBase::do_make_node_prop();
    prop->add_flag(NodeProp::Flag::CROSS_COMP_NODE_MEMORY);
    return prop;
}

MGB_IMPL_OPR_GRAD(RemoteSend) {
    mgb_assert(opr.peer_desc().is_grad);
    return RemoteRecv::make({opr.peer_desc().key + ":grad",
                             RemoteIOBase::Type::RECV, false},
                            *opr.owner_graph(), opr.group_client(),
                            OperatorNodeConfig{opr.comp_node()}.name(
                                    opr.name() + ":grad_recv"),
                            opr.input(0)->shape(), opr.input(0)->dtype())
            .node();
}

/* ===================== RemoteRecv ===================== */

MGB_DYN_TYPE_OBJ_FINAL_IMPL(RemoteRecv);

RemoteRecv::RemoteRecv(const PeerDesc& peer, cg::ComputingGraph& graph,
                       std::shared_ptr<GroupClient> group_client,
                       const OperatorNodeConfig& config,
                       const TensorShape& shape, DType dtype) :
        Super(&graph, config, "remote_recv", {}),
        m_shape(shape), m_dtype(dtype) {
    m_peer = peer;
    m_group_client = group_client;

    add_output(None)
            ->dtype(dtype)
            .add_flag(VarNode::Flag::NO_MEM_RECLAIM)
            .add_flag(VarNode::Flag::DISALLOW_RT_FORCE_DYNAMIC_MEM_ALLOC);
    add_equivalence_component<ScalarHash<void*>>(this);
}

SymbolVar RemoteRecv::make(const PeerDesc& peer, cg::ComputingGraph& graph,
                           std::shared_ptr<GroupClient> group_client,
                           const OperatorNodeConfig& config,
                           const TensorShape& shape, DType dtype) {
    auto opr = graph.insert_opr(std::make_unique<RemoteRecv>(
            peer, graph, group_client, config, shape, dtype));
    return opr->output(0);
}

void RemoteRecv::scn_do_execute() {
    if (!m_init) {
        auto&& comp_node = output(0)->comp_node();

        // rank 1 for RemoteRecv
        auto reg_info = m_group_client->opr_register(
                m_peer.key, 2, false, 1,
                comp_node.get_uid());

        m_megray_comm = MegRayCommBuilder::get_megray_comm(
                reg_info.hash, m_peer.key, 2, 1, MegRay::MEGRAY_UCX, m_group_client);

        m_megray_ctx = MegRay::CudaContext::make(get_stream(output(0)));

        m_init = true;
    }

    mgb_assert(m_init);
    size_t data_size = 1;
    auto&& tensor = output(0)->dev_tensor();
    auto&& ishp = tensor.shape();
    for (size_t i = 0; i < ishp.ndim; i++) {
        data_size *= ishp[i];
    }
    data_size *= tensor.dtype().size();
    auto status = m_megray_comm->recv(tensor.raw_ptr(), data_size, 0, m_megray_ctx);
    mgb_assert(status == MegRay::MEGRAY_OK, "MegRay recv failed");
}

void RemoteRecv::init_output_static_infer_desc() {
    using namespace cg::static_infer;
    auto&& mgr = owner_graph()->static_infer_manager();
    auto do_infer = [this](TensorShape& dest, const InpVal&) {
        dest = m_shape;
        return true;
    };
    mgr.register_shape_infer(output(0), {SourceType::CONSTANT, {}, do_infer});
}

cg::OperatorNodeBase::NodeProp* RemoteRecv::do_make_node_prop() const {
    auto prop = RemoteIOBase::do_make_node_prop();
    prop->add_flag(NodeProp::Flag::IMPURE_FUNC);
    if (input().size() == 1)
        prop->reset_dep_type(input(), {NodeProp::DepType::DEV_COMP_ORDER});
    return prop;
}

/* ===================== shallow copy ===================== */

namespace mgb {
namespace opr {

cg::OperatorNodeBase* opr_shallow_copy_remote_send(
        const serialization::OprShallowCopyContext& ctx,
        const cg::OperatorNodeBase& opr_, const VarNodeArray& inputs,
        const OperatorNodeConfig& config) {
    mgb_assert(inputs.size() == 1);
    auto&& opr = opr_.cast_final_safe<RemoteSend>();
    return RemoteSend::make(opr.peer_desc(), inputs[0], opr.group_client(),
                            config)
            .node()
            ->owner_opr();
}
MGB_REG_OPR_SHALLOW_COPY(RemoteSend, opr_shallow_copy_remote_send);

cg::OperatorNodeBase* opr_shallow_copy_remote_recv(
        const serialization::OprShallowCopyContext& ctx,
        const cg::OperatorNodeBase& opr_, const VarNodeArray& inputs,
        const OperatorNodeConfig& config) {
    auto&& opr = opr_.cast_final_safe<RemoteRecv>();
    return RemoteRecv::make(opr.peer_desc(), *opr.owner_graph(),
                            opr.group_client(), config, inputs[0]->shape(),
                            inputs[0]->dtype())
            .node()
            ->owner_opr();
}
MGB_REG_OPR_SHALLOW_COPY(RemoteRecv, opr_shallow_copy_remote_recv);

}  // namespace opr
}  // namespace mgb

// vim: syntax=cpp.doxygen foldmethod=marker foldmarker=f{{{,f}}}
