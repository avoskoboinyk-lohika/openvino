// Copyright (C) 2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <string>
#include <vector>
#include <math.h>
#include <dnnl_types.h>
#include "dnnl_extension_utils.h"
#include <cpu/x64/jit_generator.hpp>
#include <cpu/x64/injectors/jit_uni_eltwise_injector.hpp>
#include "ie_parallel.hpp"
#include "memory_desc/dnnl_blocked_memory_desc.h"
#include "nodes/kernels/jit_kernel_base.hpp"
#include "nodes/kernels/jit_kernel_traits.hpp"

namespace ov {
namespace intel_cpu {

using namespace InferenceEngine;
using namespace dnnl::impl;
using namespace dnnl::impl::utils;
using namespace dnnl::impl::cpu::x64;
using namespace dnnl::impl::cpu::x64::eltwise_injector;

struct jit_refine_anchors_conf {
    int32_t anchors_chunk;
};

struct jit_refine_anchors_call_args {
    const float* deltas;
    const float* scores;
    const float* anchors;
    float* proposals;
    const int32_t h;
    const int32_t w;
    const int32_t anchors_num;
    const int32_t* refine_anchor_indices;
    const uint32_t* refine_anchor_masks;
    uint32_t anchor_start_idx;
    uint32_t anchor_anchor_offset;
    uint32_t anchor_idx_offset;
    uint32_t delta_start_idx;
    uint32_t delta_anchor_offset;
    uint32_t delta_idx_offset;
    uint32_t score_start_idx;
    uint32_t score_anchor_offset;
    uint32_t proposal_start_idx;
    uint32_t proposal_anchor_offset;
    uint32_t proposal_idx_offset;
    const float img_h;
    const float img_w;
    const float min_box_h;
    const float min_box_w;
    const float max_delta_log_wh;
    const float coordinates_offset;
};

using jit_refine_anchors_kernel = jit_kernel_base<jit_refine_anchors_conf, jit_refine_anchors_call_args>;

template <x64::cpu_isa_t isa>
class jit_refine_anchors_kernel_fp32 : public jit_refine_anchors_kernel {
 public:
    DECLARE_CPU_JIT_AUX_FUNCTIONS(jit_refine_anchors_kernel_fp32)

    static constexpr auto KERNEL_ELEMENT_TYPE = ov::element::Type_t::f32;

    using Vmm = typename jit_kernel_traits<isa, KERNEL_ELEMENT_TYPE>::Vmm;
    static constexpr unsigned VCMPPS_LE = 0x02;
    static constexpr unsigned VCMPPS_GT = 0x0e;
//    using Vmm = Xbyak::Xmm;
    static constexpr unsigned XMM_SIMD_WIDTH = 16 / sizeof(typename ov::element_type_traits<KERNEL_ELEMENT_TYPE>::value_type);
    static constexpr unsigned YMM_SIMD_WIDTH = 32 / sizeof(typename ov::element_type_traits<KERNEL_ELEMENT_TYPE>::value_type);
    static constexpr unsigned ZMM_SIMD_WIDTH = 64 / sizeof(typename ov::element_type_traits<KERNEL_ELEMENT_TYPE>::value_type);
    static constexpr unsigned DTYPE_SIZE = sizeof(typename ov::element_type_traits<KERNEL_ELEMENT_TYPE>::value_type);
    static constexpr unsigned SIMD_WIDTH = jit_kernel_traits<isa, KERNEL_ELEMENT_TYPE>::SIMD_WIDTH;
//    static constexpr unsigned SIMD_WIDTH = XMM_SIMD_WIDTH;


    jit_refine_anchors_kernel_fp32(const jit_refine_anchors_conf &jqp)
        : jit_refine_anchors_kernel(isa, jqp) {}

    void generate() override;

 private:
//    const size_t xmm_len = 16;
    
    void update_input_output_ptrs();

//    inline void push_xmm(const Xbyak::Xmm &xmm) {
//        sub(rsp, xmm_len);
//        uni_vmovdqu(ptr[rsp], xmm);
//    }
//
//    inline void push_xmm(const std::vector<Xbyak::Xmm> &xmms) {
//        sub(rsp, xmms.size() * xmm_len);
//        for (size_t i = 0; i < xmms.size(); ++i) {
//            uni_vmovdqu(ptr[rsp + i * xmm_len], xmms[i]);
//        }
//    }
//
//    inline void pop_xmm(const Xbyak::Xmm &xmm) {
//        uni_vmovdqu(xmm, ptr[rsp]);
//        add(rsp, xmm_len);
//    }
//
//    inline void pop_xmm(const std::vector<Xbyak::Xmm> &xmms) {
//        for (size_t i = 0; i < xmms.size(); ++i) {
//            uni_vmovdqu(xmms[i], ptr[rsp + i * xmm_len]);
//        }
//        sub(rsp, xmms.size() * xmm_len);
//    }

    template<typename Tmm>
    inline Tmm get_free_vmm_const(const std::vector<Xbyak::Xmm>& not_available) {
        static_assert(std::is_base_of<Xbyak::Xmm, Tmm>::value, "Xbyak::Xmm should be base of Tmm");
        std::vector<int> xmm_idxs{
                xmm0.getIdx(), xmm1.getIdx(), xmm2.getIdx(), xmm3.getIdx(), xmm4.getIdx(), xmm5.getIdx(), xmm6.getIdx(), xmm7.getIdx(),
#ifdef XBYAK64
                xmm8.getIdx(), xmm9.getIdx(), xmm10.getIdx(), xmm11.getIdx(), xmm12.getIdx(), xmm13.getIdx(), xmm14.getIdx(), xmm15.getIdx()
#endif
        };
        std::vector<int> not_available_idx(not_available.size());
        std::transform(not_available.begin(), not_available.end(), not_available_idx.begin(),
                       [](const Xbyak::Xmm& xmm) {
                           return xmm.getIdx();
                       });
        auto removed = std::remove_if(xmm_idxs.begin(), xmm_idxs.end(),
                                      [&not_available_idx](const int& xmm_idx) {
                                          return not_available_idx.end() != std::find(not_available_idx.begin(), not_available_idx.end(), xmm_idx);
                                      });
        xmm_idxs.erase(removed, xmm_idxs.end());
        return Tmm{xmm_idxs.front()};
    }

    template<typename Tmm>
    inline Tmm get_free_vmm(std::vector<Xbyak::Xmm>& not_available) {
        auto available_tmm = this->get_free_vmm_const<Tmm>(not_available);
        not_available.push_back(available_tmm);
        return available_tmm;
    }

    template<typename TReg>
    inline TReg get_free_reg_const(const std::vector<Xbyak::Reg>& not_available) {
        static_assert(std::is_base_of<Xbyak::Reg, TReg>::value, "Reg::Reg should be base of Tmm");
        std::vector<int> reg_idxs{
                rax.getIdx(), rcx.getIdx(), rdx.getIdx(), rbx.getIdx(), /*rsp,*/ rbp.getIdx(), rsi.getIdx(), rdi.getIdx(),
                r8.getIdx(), r9.getIdx(), r10.getIdx(), r11.getIdx(), r12.getIdx(), r13.getIdx(), r14.getIdx(), r15.getIdx()
        };
        std::vector<int> not_available_idx(not_available.size());
        std::transform(not_available.begin(), not_available.end(), not_available_idx.begin(),
                       [](const Xbyak::Reg& reg) {
                           return reg.getIdx();
                       });
        auto removed = std::remove_if(reg_idxs.begin(), reg_idxs.end(),
                                      [&not_available_idx](const int& reg_idx) {
                                          return not_available_idx.end() != std::find(not_available_idx.begin(), not_available_idx.end(), reg_idx);
                                      });
        reg_idxs.erase(removed, reg_idxs.end());
        return TReg{reg_idxs.front()};
    }

    template<typename Tmm>
    inline Tmm get_free_reg(std::vector<Xbyak::Reg>& not_available) {
        auto available_reg = this->get_free_reg_const<Tmm>(not_available);
        not_available.push_back(available_reg);
        return available_reg;
    }

    inline void uni_vgatherdps(const Xbyak::Xmm &xmm_val,
                               const Xbyak::Reg64 &reg_addr,
                               const Xbyak::Xmm &xmm_index,
                               const int &scale,
                               const Xbyak::Reg &reg_mask) {
        assert(scale != 0);
        const size_t scale_remainder = scale % 4;
        if (mayiuse(cpu_isa_t::avx512_core) && (0 == scale_remainder)) {
            assert(reg_mask.isOPMASK());
            vgatherdps(xmm_val, ptr[reg_addr + xmm_index * scale]);
        } else if (mayiuse(cpu_isa_t::avx2) && (0 == scale_remainder)) {
            assert(reg_mask.isYMM());
            Xbyak::Ymm ymm_mask{reg_mask.getIdx()};
            vgatherdps(xmm_val, ptr[reg_addr + xmm_index * scale], ymm_mask);
        } else {
            assert(reg_mask.isXMM());
            Xbyak::Xmm xmm_mask{reg_mask.getIdx()};
            assert(xmm_val.getKind() == xmm_index.getKind());
            assert(xmm_index.getKind() == xmm_mask.getKind());

            std::vector<Xbyak::Reg> not_available_reg{reg_addr};
            const Xbyak::Reg64 idx = this->get_free_reg<Xbyak::Reg64>(not_available_reg);
            const Xbyak::Reg64 mask = this->get_free_reg<Xbyak::Reg64>(not_available_reg);

            push(idx);
            push(mask);
            xor_(idx, idx);
            xor_(mask, mask);

            for (int i = 0; i < SIMD_WIDTH; i++) {
                Xbyak::Label gather_end;
                uni_vpextrd(mask.cvt32(), xmm_mask, i);
                cmp(mask.cvt32(), 0xFFFFFFFF);
                jne(gather_end, T_NEAR);
                uni_vpextrd(idx.cvt32(), xmm_index, i);
                Xbyak::Address addr = ptr[reg_addr + idx * scale];
                switch (scale) {
                    case 8: uni_vpinsrq(xmm_val, xmm_val, addr, i); break;
                    case 4: uni_vpinsrd(xmm_val, xmm_val, addr, i); break;
                    case 2: uni_vpinsrw(xmm_val, xmm_val, addr, i); break;
                    case 1: uni_vpinsrb(xmm_val, xmm_val, addr, i); break;
                    default: IE_THROW() << "The data type of size '" << scale << "' is not supported.";
                }
                L(gather_end);
            }
            pop(mask);
            pop(idx);
        }
    }

    inline void uni_vscatterdps(const Xbyak::Reg64 &reg_addr,
                                const Xbyak::Xmm &xmm_index,
                                const int &scale,
                                const Xbyak::Xmm &xmm_val,
                                const Xbyak::Reg &reg_mask) {
        assert(scale != 0);
        const size_t scale_remainder = scale % 4;
        if (mayiuse(cpu_isa_t::avx512_core) && (0 == scale_remainder)) {
            assert(reg_mask.isOPMASK());
            vscatterdps(ptr[reg_addr + xmm_index * scale], xmm_val);
        } else {
            assert(reg_mask.isXMM() || reg_mask.isYMM());
            Vmm xmm_mask{reg_mask.getIdx()};
            assert(xmm_val.getKind() == xmm_index.getKind());
            assert(xmm_index.getKind() == xmm_mask.getKind());

            std::vector<Xbyak::Reg> not_available_reg{reg_addr};
            std::vector<Xbyak::Xmm> not_available_xmm{xmm_index, xmm_val, Xbyak::Xmm{reg_mask.getIdx()}};
            const Xbyak::Reg64 idx = this->get_free_reg<Xbyak::Reg64>(not_available_reg);
            const Xbyak::Reg64 mask = this->get_free_reg<Xbyak::Reg64>(not_available_reg);
            const Xbyak::Reg64 val = this->get_free_reg<Xbyak::Reg64>(not_available_reg);
            const Xbyak::Xmm xmm_val_temp = this->get_free_vmm<Xbyak::Xmm>(not_available_xmm);

            push(idx);
            push(mask);
            push(val);
            push_xmm(xmm_val_temp);
            xor_(idx, idx);
            xor_(mask, mask);
            xor_(val, val);

            auto extract_xmm = [this](const Xbyak::Reg64 in, const Xbyak::Xmm& xmm_val_temp, const Xbyak::Xmm& xmm_reg, const int& i, const int &scale) {
                if (mayiuse(cpu_isa_t::avx2)) {
                    vextracti128(xmm_val_temp, Xbyak::Ymm{xmm_reg.getIdx()}, i/scale);
                    uni_vpextrd(in.cvt32(), xmm_val_temp, i%scale);
                } else {
                    uni_vpextrd(in.cvt32(), xmm_reg, i);
                }
            };

            for (int i = 0; i < SIMD_WIDTH; i++) {
                Xbyak::Label scatter_end;
                extract_xmm(mask, xmm_val_temp, xmm_mask, i, scale);
                cmp(mask.cvt32(), 0xFFFFFFFF);
                jne(scatter_end, T_NEAR);
                extract_xmm(idx, xmm_val_temp, xmm_index, i, scale);
                Xbyak::Address addr = ptr[reg_addr + idx * scale];
                switch (scale) {
                    case 8: uni_vpextrq(val, xmm_val, i); mov(addr, val); break;
                    case 4: extract_xmm(val, xmm_val_temp, xmm_val, i, scale); mov(addr, val.cvt32()); break;
                    case 2: uni_vpextrw(val.cvt16(), xmm_val, i); mov(addr, val.cvt16()); break;
                    case 1: uni_vpextrb(val.cvt8(), xmm_val, i); mov(addr, val.cvt8()); break;
                    default: IE_THROW() << "The data type of size '" << scale << "' is not supported.";
                }
                L(scatter_end);
            }
            pop_xmm(xmm_val_temp);
            pop(val);
            pop(mask);
            pop(idx);
        }
    }

    template<typename Tmm>
    void uni_expf(const Tmm& arg) {
        exp_injector->compute_vector(arg.getIdx());
    }

    std::shared_ptr<jit_uni_eltwise_injector_f32<isa>> exp_injector =
        std::make_shared<jit_uni_eltwise_injector_f32<isa>>(this, dnnl::impl::alg_kind::eltwise_exp, 0.f, 0.f, 1.f);

    Xbyak::Reg64 reg_params = abi_param1;

    Xbyak::Reg64 reg_anchors_loop = rcx;

    // Stable variables
    Xbyak::Reg64 reg_anchors_ptr = r8;
    Xbyak::Reg64 reg_deltas_ptr = r9;
    Xbyak::Reg64 reg_scores_ptr = r10;
    Xbyak::Reg64 reg_proposals_ptr = r11;
    Xbyak::Reg64 reg_anchors_chunk = r12;
    Xbyak::Reg64 reg_img_h = r13;
    Xbyak::Reg64 reg_img_w = r14;
    Xbyak::Reg64 reg_num_proc_elem = r15;

    Vmm vmm_x0 = Vmm(0);
    Vmm vmm_y0 = Vmm(1);
    Vmm vmm_x1 = Vmm(2);
    Vmm vmm_y1 = Vmm(3);
    Vmm vmm_dx = Vmm(4);
    Vmm vmm_dy = Vmm(5);
    Vmm vmm_d_log_w = Vmm(6);
    Vmm vmm_d_log_h = Vmm(7);


};

//template <x64::cpu_isa_t isa>
//template <>
//struct jit_refine_anchors_kernel_fp32<isa>::jit_traits<x64::sse41> {
//    using Vmm = Xbyak::Xmm;
//    static constexpr unsigned SIMD_WIDTH = x64::cpu_isa_traits<x64::sse41>::vlen / sizeof(typename ov::element_type_traits<KERNEL_ELEMENT_TYPE>::value_type);
//};

}
}
