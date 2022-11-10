// Copyright (C) 2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "jit_refine_anchors_kernel.hpp"

namespace ov {
namespace intel_cpu {

/**
 * @code
 */
template <x64::cpu_isa_t isa>
void jit_refine_anchors_kernel_fp32<isa>::generate() {
    /** @code
        for (int anchor = 0; anchor < anchors_num; ++anchor) {
            const int a_idx = anchor_idx(h, w, anchor, 0);
            const int a_idx_offset = anchor_idx(h, w, anchor, 1) - anchor_idx(h, w, anchor, 0);
            float x0 = anchors[a_idx + 0 * a_idx_offset];
            float y0 = anchors[a_idx + 1 * a_idx_offset];
            float x1 = anchors[a_idx + 2 * a_idx_offset];
            float y1 = anchors[a_idx + 3 * a_idx_offset];

            const int d_idx = delta_idx(anchor, 0, h, w);
            const int d_idx_offset = delta_idx(anchor, 1, h, w) - delta_idx(anchor, 0, h, w);
            const float dx = deltas[d_idx + 0 * d_idx_offset];
            const float dy = deltas[d_idx + 1 * d_idx_offset];
            const float d_log_w = deltas[d_idx + 2 * d_idx_offset];
            const float d_log_h = deltas[d_idx + 3 * d_idx_offset];

            const float score = scores[score_idx(anchor, 0, h, w)];

            // width & height of box
            const float ww = x1 - x0 + coordinates_offset;
            const float hh = y1 - y0 + coordinates_offset;
            // center location of box
            const float ctr_x = x0 + 0.5f * ww;
            const float ctr_y = y0 + 0.5f * hh;

            // new center location according to deltas (dx, dy)
            const float pred_ctr_x = dx * ww + ctr_x;
            const float pred_ctr_y = dy * hh + ctr_y;
            // new width & height according to deltas d(log w), d(log h)
            const float pred_w = std::exp(std::min(d_log_w, max_delta_log_wh)) * ww;
            const float pred_h = std::exp(std::min(d_log_h, max_delta_log_wh)) * hh;

            // update upper-left corner location
            x0 = pred_ctr_x - 0.5f * pred_w;
            y0 = pred_ctr_y - 0.5f * pred_h;
            // update lower-right corner location
            x1 = pred_ctr_x + 0.5f * pred_w - coordinates_offset;
            y1 = pred_ctr_y + 0.5f * pred_h - coordinates_offset;

            // adjust new corner locations to be within the image region,
            x0 = std::max<float>(0.0f, std::min<float>(x0, img_w - coordinates_offset));
            y0 = std::max<float>(0.0f, std::min<float>(y0, img_h - coordinates_offset));
            x1 = std::max<float>(0.0f, std::min<float>(x1, img_w - coordinates_offset));
            y1 = std::max<float>(0.0f, std::min<float>(y1, img_h - coordinates_offset));

            // recompute new width & height
            const float box_w = x1 - x0 + coordinates_offset;
            const float box_h = y1 - y0 + coordinates_offset;

            int p_idx = proposal_idx(h, w, anchor, 0);
            proposals[p_idx + 0] = x0;
            proposals[p_idx + 1] = y0;
            proposals[p_idx + 2] = x1;
            proposals[p_idx + 3] = y1;
            proposals[p_idx + 4] = score;
            proposals[p_idx + 5] = (min_box_w <= box_w) * (min_box_h <= box_h) * 1.0;
        }
     */

    this->preamble();

    xor_(reg_anchors_loop, reg_anchors_loop);
    mov(reg_anchors_loop.cvt32(), ptr[reg_params + offsetof(jit_refine_anchors_call_args, anchors_num)]);
    mov(reg_anchors_ptr, ptr[reg_params + offsetof(jit_refine_anchors_call_args, anchors)]);
    mov(reg_deltas_ptr, ptr[reg_params + offsetof(jit_refine_anchors_call_args, deltas)]);
    mov(reg_scores_ptr, ptr[reg_params + offsetof(jit_refine_anchors_call_args, scores)]);
    mov(reg_proposals_ptr, ptr[reg_params + offsetof(jit_refine_anchors_call_args, proposals)]);
//    mov(reg_anchors_offset, ptr[reg_params + offsetof(jit_refine_anchors_call_args, anchors_idx_offset)]);
//    mov(reg_delta_index_ptr, ptr[reg_params + offsetof(jit_refine_anchors_call_args, delta_idx)]);
//    mov(reg_delta_offset, ptr[reg_params + offsetof(jit_refine_anchors_call_args, delta_idx_offset)]);

    Xbyak::Label anchor_loop;
    Xbyak::Label loop_mask;
    L(anchor_loop);
    {
        mov(reg_anchors_chunk, this->SIMD_WIDTH);
        cmp(reg_anchors_loop, this->SIMD_WIDTH);
        jae(loop_mask);
        mov(reg_anchors_chunk, reg_anchors_loop);
        L(loop_mask);

        /** @code
            const int a_idx = anchor_idx(h, w, anchor, 0);
            const int a_idx_offset = anchor_idx(h, w, anchor, 1) - anchor_idx(h, w, anchor, 0);
            float x0 = anchors[a_idx + 0 * a_idx_offset];
            float y0 = anchors[a_idx + 1 * a_idx_offset];
            float x1 = anchors[a_idx + 2 * a_idx_offset];
            float y1 = anchors[a_idx + 3 * a_idx_offset];
         */
        xor_(reg_anchor_chunk_offset, reg_anchor_chunk_offset);
        mov(reg_anchor_chunk_offset.cvt32(), ptr[reg_params + offsetof(jit_refine_anchors_call_args, anchor_chunk_offset)]);
        // float x0 = anchors[a_idx + 0 * a_idx_offset];
        mov(reg_anchor_idx, 0);
        emulate_gather(vmm_x0, reg_anchors_chunk, reg_anchors_ptr, reg_anchor_chunk_offset, reg_anchor_idx);
        // float y0 = anchors[a_idx + 1 * a_idx_offset];
        mov(reg_anchor_idx, 1);
        emulate_gather(vmm_y0, reg_anchors_chunk, reg_anchors_ptr, reg_anchor_chunk_offset, reg_anchor_idx);
        // float x1 = anchors[a_idx + 2 * a_idx_offset];
        mov(reg_anchor_idx, 2);
        emulate_gather(vmm_x1, reg_anchors_chunk, reg_anchors_ptr, reg_anchor_chunk_offset, reg_anchor_idx);
        // float y1 = anchors[a_idx + 3 * a_idx_offset];
        mov(reg_anchor_idx, 3);
        emulate_gather(vmm_y1, reg_anchors_chunk, reg_anchors_ptr, reg_anchor_chunk_offset, reg_anchor_idx);

        /** @code
            const int d_idx = delta_idx(anchor, 0, h, w);
            const int d_idx_offset = delta_idx(anchor, 1, h, w) - delta_idx(anchor, 0, h, w);
            const float dx = deltas[d_idx + 0 * d_idx_offset];
            const float dy = deltas[d_idx + 1 * d_idx_offset];
            const float d_log_w = deltas[d_idx + 2 * d_idx_offset];
            const float d_log_h = deltas[d_idx + 3 * d_idx_offset];
         */
        xor_(reg_delta_chunk_offset, reg_delta_chunk_offset);
        xor_(reg_delta_idx_offset, reg_delta_idx_offset);
        mov(reg_delta_chunk_offset.cvt32(), ptr[reg_params + offsetof(jit_refine_anchors_call_args, delta_chunk_offset)]);
        mov(reg_delta_idx_offset.cvt32(), ptr[reg_params + offsetof(jit_refine_anchors_call_args, delta_idx_offset)]);
        // const float dx = deltas[d_idx + 0 * d_idx_offset];
        mov(reg_anchor_idx, 0);
        add(reg_anchor_idx, ptr[reg_params + offsetof(jit_refine_anchors_call_args, img_w)]);
        emulate_gather(vmm_dx, reg_anchors_chunk, reg_deltas_ptr, reg_delta_chunk_offset, reg_anchor_idx);
        // const float dy = deltas[d_idx + 1 * d_idx_offset];
        mov(reg_anchor_idx, 2);
        imul(reg_anchor_idx, reg_delta_idx_offset);
        add(reg_anchor_idx, ptr[reg_params + offsetof(jit_refine_anchors_call_args, img_w)]);
        emulate_gather(vmm_dy, reg_anchors_chunk, reg_deltas_ptr, reg_delta_chunk_offset, reg_anchor_idx);
        // const float d_log_w = deltas[d_idx + 2 * d_idx_offset];
        mov(reg_anchor_idx, 3);
        imul(reg_anchor_idx, reg_delta_idx_offset);
        add(reg_anchor_idx, ptr[reg_params + offsetof(jit_refine_anchors_call_args, img_w)]);
        emulate_gather(vmm_d_log_w, reg_anchors_chunk, reg_deltas_ptr, reg_delta_chunk_offset, reg_anchor_idx);
        // const float d_log_h = deltas[d_idx + 2 * d_idx_offset];
        mov(reg_anchor_idx, 4);
        imul(reg_anchor_idx, reg_delta_idx_offset);
        add(reg_anchor_idx, ptr[reg_params + offsetof(jit_refine_anchors_call_args, img_w)]);
        emulate_gather(vmm_d_log_h, reg_anchors_chunk, reg_deltas_ptr, reg_delta_chunk_offset, reg_anchor_idx);

//        /** @code
//            // width & height of box
//            const float ww = x1 - x0 + coordinates_offset;
//            const float hh = y1 - y0 + coordinates_offset;
//         */
//        // const float ww = x1 - x0 + coordinates_offset;
//        uni_vaddss(vmm_ww, vmm_x0, reg_coordinates_offset);
//        uni_vsubps(vmm_ww, vmm_ww, vmm_x1);
//        // const float hh = y1 - y0 + coordinates_offset;
//        uni_vaddss(vmm_hh, vmm_y0, reg_coordinates_offset);
//        uni_vsubps(vmm_hh, vmm_hh, vmm_y1);
//
//        /** @code
//            // center location of box
//            const float ctr_x = x0 + 0.5f * ww;
//            const float ctr_y = y0 + 0.5f * hh;
//         */
//        // const float ctr_x = x0 + 0.5f * ww;
//        uni_vmulss(vmm_ctr_x, vmm_ww, reg_scale_0_5);
//        uni_vaddps(vmm_ctr_x, vmm_ctr_x, vmm_x0);
//        // const float ctr_y = y0 + 0.5f * hh;
//        uni_vmulss(xmm_ctr_y, vmm_hh, reg_scale_0_5);
//        uni_vaddps(xmm_ctr_y, xmm_ctr_y, vmm_y0);
//
//        /** @code
//            // new center location according to deltas (dx, dy)
//            const float pred_ctr_x = dx * ww + ctr_x;
//            const float pred_ctr_y = dy * hh + ctr_y;
//         */
//        // const float pred_ctr_x = dx * ww + ctr_x;
//        uni_vmulps(xmm_pred_ctr_x, xmm_dx, vmm_ww);
//        uni_vaddps(xmm_pred_ctr_x, xmm_pred_ctr_x, vmm_ctr_x);
//        // const float pred_ctr_y = dy * hh + ctr_y;
//        uni_vmulps(xmm_pred_ctr_y, xmm_dy, vmm_hh);
//        uni_vaddps(xmm_pred_ctr_y, xmm_pred_ctr_y, xmm_ctr_y);
//
//        /** @code
//            // new width & height according to deltas d(log w), d(log h)
//            const float pred_w = std::exp(std::min(d_log_w, max_delta_log_wh)) * ww;
//            const float pred_h = std::exp(std::min(d_log_h, max_delta_log_wh)) * hh;
//         */
//        // const float pred_w = std::exp(std::min(d_log_w, max_delta_log_wh)) * ww;
//        uni_vminss(xmm_pred_w, xmm_d_log_w, xmm_max_delta_log_wh);
//        uni_expf(vmm_pred_w);
//        uni_vmulps(xmm_pred_w, xmm_pred_w, vmm_ww);
//        // const float pred_h = std::exp(std::min(d_log_h, max_delta_log_wh)) * hh;
//        uni_vminss(xmm_pred_h, xmm_d_log_h, xmm_max_delta_log_wh);
//        uni_expf(vmm_pred_h);
//        uni_vmulps(xmm_pred_h, xmm_pred_h, vmm_hh);
//
//        /** @code
//            // update upper-left corner location
//            x0 = pred_ctr_x - 0.5f * pred_w;
//            y0 = pred_ctr_y - 0.5f * pred_h;
//         */
//        // x0 = pred_ctr_x - 0.5f * pred_w;
//        uni_vmulss(vmm_x0, xmm_pred_w, reg_scale_0_5);
//        uni_vaddps(vmm_x0, xmm_pred_ctr_x, vmm_x0);
//        // y0 = pred_ctr_y - 0.5f * pred_h;
//        uni_vmulss(vmm_y0, xmm_pred_h, reg_scale_0_5);
//        uni_vaddps(vmm_y0, xmm_pred_ctr_y, vmm_y0);
//
//        /** @code
//            // update lower-right corner location
//            x1 = pred_ctr_x + 0.5f * pred_w - coordinates_offset;
//            y1 = pred_ctr_y + 0.5f * pred_h - coordinates_offset;
//         */
//        // x1 = pred_ctr_x + 0.5f * pred_w - coordinates_offset;
//        uni_vmulss(vmm_x1, xmm_pred_w, reg_scale_0_5);
//        uni_vsubss(vmm_x1, vmm_x1, reg_coordinates_offset);
//        uni_vaddps(vmm_x1, xmm_pred_ctr_x, vmm_x1);
//        // y1 = pred_ctr_y + 0.5f * pred_h - coordinates_offset;
//        uni_vmulss(vmm_y1, xmm_pred_h, reg_scale_0_5);
//        uni_vsubss(vmm_y1, vmm_y1, reg_coordinates_offset);
//        uni_vaddps(vmm_y1, xmm_pred_ctr_y, vmm_y1);
//
//        sub(reg_img_W, reg_coordinates_offset);
//        sub(reg_img_H, reg_coordinates_offset);
//        /** @code
//            // adjust new corner locations to be within the image region,
//            x0 = std::max<float>(0.0f, std::min<float>(x0, img_w - coordinates_offset));
//            y0 = std::max<float>(0.0f, std::min<float>(y0, img_h - coordinates_offset));
//         */
//        // x0 = std::max<float>(0.0f, std::min<float>(x0, img_w - coordinates_offset));
//        uni_vminss(vmm_x0, vmm_x0, reg_img_W);
//        uni_vmaxss(vmm_x0, xmm_0_0, vmm_x0);
//        // y0 = std::max<float>(0.0f, std::min<float>(y0, img_h - coordinates_offset));
//        uni_vminss(vmm_y0, vmm_y0, reg_img_H);
//        uni_vmaxss(vmm_y0, xmm_0_0, vmm_y0);
//
//        /** @code
//            // adjust new corner locations to be within the image region,
//            x1 = std::max<float>(0.0f, std::min<float>(x1, img_w - coordinates_offset));
//            y1 = std::max<float>(0.0f, std::min<float>(y1, img_h - coordinates_offset));
//         */
//        // x1 = std::max<float>(0.0f, std::min<float>(x1, img_w - coordinates_offset));
//        uni_vminss(vmm_x1, vmm_x1, reg_img_W);
//        uni_vmaxss(vmm_x1, xmm_0_0, vmm_x1);
//        // y1 = std::max<float>(0.0f, std::min<float>(y1, img_h - coordinates_offset));
//        uni_vminss(vmm_y1, vmm_y1, reg_img_H);
//        uni_vmaxss(vmm_y1, xmm_0_0, vmm_y1);
//
//        /** @code
//            // recompute new width & height
//            const float box_w = x1 - x0 + coordinates_offset;
//            const float box_h = y1 - y0 + coordinates_offset;
//         */
//        // const float box_w = x1 - x0 + coordinates_offset;
//        uni_vaddss(xmm_box_w, vmm_x0, reg_coordinates_offset);
//        uni_vaddps(xmm_box_w, vmm_x1, xmm_box_w);
//        // const float box_h = y1 - y0 + coordinates_offset;
//        uni_vaddss(xmm_box_h, vmm_y0, reg_coordinates_offset);
//        uni_vaddps(xmm_box_h, vmm_y1, xmm_box_h);
//

        /** @code
            const float score = scores[score_idx(anchor, 0, h, w)];
         */
        xor_(reg_score_idx_offset, reg_score_idx_offset);
        mov(reg_score_idx_offset.cvt32(), ptr[reg_params + offsetof(jit_refine_anchors_call_args, score_chunk_offset)]);
        // const float score = scores[score_idx(anchor, 0, h, w)];
        mov(reg_anchor_idx, 0);
        emulate_gather(vmm_score, reg_anchors_chunk, reg_scores_ptr, reg_anchor_idx_offset, reg_anchor_idx);

//        /** @code
//            int p_idx = proposal_idx(h, w, anchor, 0);
//            proposals[p_idx + 0] = x0;
//            proposals[p_idx + 1] = y0;
//            proposals[p_idx + 2] = x1;
//            proposals[p_idx + 3] = y1;
//            proposals[p_idx + 4] = score;
//            proposals[p_idx + 5] = (min_box_w <= box_w) * (min_box_h <= box_h) * 1.0;
//         */
//        mov(reg_proposal_idx_offset, ptr[reg_params + offsetof(jit_refine_anchors_call_args, proposal_idx_offset)]);
//        mov(reg_proposal_chunk_offset, ptr[reg_params + offsetof(jit_refine_anchors_call_args, proposal_chunk_offset)]);
//        // int p_idx = proposal_idx(h, w, anchor, 0);
//        uni_vmovdqu(xmm_proposals_index, ptr[reg_proposals_index]);
//        uni_vpcmpeqd(vmm_mask, vmm_mask, vmm_mask);
//        // proposals[p_idx + 0] = x0;
//        vscatterdps(ptr[reg_proposals_ptr + xmm_proposals_index], vmm_x0);
//        // proposals[p_idx + 1] = y0;
//        vscatterdps(ptr[reg_proposals_ptr + xmm_proposals_index], vmm_y0);
//        // proposals[p_idx + 2] = x1;
//        vscatterdps(ptr[reg_proposals_ptr + xmm_proposals_index], vmm_x1);
//        // proposals[p_idx + 3] = y1;
//        vscatterdps(ptr[reg_proposals_ptr + xmm_proposals_index], vmm_y1);
//        // proposals[p_idx + 4] = score;
//        vscatterdps(ptr[reg_proposals_ptr + xmm_proposals_index], xmm_score);
//        // proposals[p_idx + 5] = (min_box_w <= box_w) * (min_box_h <= box_h) * 1.0;
//        uni_vmovdqu(xmm_min_box_w, ptr[reg_proposals_index]);
//        uni_vmovdqu(xmm_min_box_h, ptr[reg_proposals_index]);
//        vcmpeq_uqps(xmm_min_box_w, xmm_min_box_w, xmm_box_w);
//        vcmpeq_uqps(xmm_min_box_h, xmm_min_box_h, xmm_box_h);
//        uni_vmulps(xmm_min_box_w, xmm_min_box_w, xmm_min_box_h);
//        vscatterdps(ptr[reg_proposals_ptr + xmm_proposals_index], xmm_min_box_w);

        this->update_input_output_ptrs();

        sub(reg_anchors_loop, reg_anchors_chunk);
        jbe(anchor_loop);
    }

    this->postamble();

    exp_injector->prepare_table();
}

template <x64::cpu_isa_t isa>
void jit_refine_anchors_kernel_fp32<isa>::update_input_output_ptrs() {
    mov(reg_num_proc_elem, reg_anchors_chunk);
    imul(reg_num_proc_elem, reg_num_proc_elem, 4);
    mov(reg_anchor_idx_offset, ptr[reg_params + offsetof(jit_refine_anchors_call_args, anchor_idx_offset)]);
    imul(reg_num_proc_elem, reg_anchor_idx_offset);
    add(reg_anchors_ptr, reg_num_proc_elem);
    mov(reg_num_proc_elem, reg_anchors_chunk);
    imul(reg_num_proc_elem, reg_num_proc_elem, 4);
    mov(reg_delta_idx_offset, ptr[reg_params + offsetof(jit_refine_anchors_call_args, delta_idx_offset)]);
    imul(reg_num_proc_elem, reg_delta_idx_offset);
    add(reg_deltas_ptr, reg_num_proc_elem);
    mov(reg_num_proc_elem, reg_anchors_chunk);
    imul(reg_num_proc_elem, reg_num_proc_elem, 4);
    mov(reg_score_idx_offset, ptr[reg_params + offsetof(jit_refine_anchors_call_args, score_idx_offset)]);
    imul(reg_num_proc_elem, reg_score_idx_offset);
    add(reg_scores_ptr, reg_num_proc_elem);
    mov(reg_num_proc_elem, reg_anchors_chunk);
    imul(reg_num_proc_elem, reg_num_proc_elem, 4);
    mov(reg_proposal_idx_offset, ptr[reg_params + offsetof(jit_refine_anchors_call_args, proposal_idx_offset)]);
    imul(reg_num_proc_elem, reg_proposal_idx_offset);
    add(reg_proposals_ptr, reg_num_proc_elem);
}

template struct jit_refine_anchors_kernel_fp32<x64::avx512_core>;
template struct jit_refine_anchors_kernel_fp32<x64::avx2>;
template struct jit_refine_anchors_kernel_fp32<x64::sse41>;

}
}
