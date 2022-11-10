// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <cstring>
#include <cassert>
#include <cmath>
#include <string>
#include <vector>
#include <utility>
#include <algorithm>
#include <ngraph/op/generate_proposals.hpp>
#include "ie_parallel.hpp"
#include "common/cpu_memcpy.h"
#include "generate_proposals.h"
#include "proposal.h"

using namespace dnnl::impl::cpu;
using namespace dnnl::impl::cpu::x64;

namespace ov {
namespace intel_cpu {
namespace node {
namespace {

using namespace InferenceEngine;

namespace seq {
void parallel_for(int first,
                  std::function<void(int)> callback);
void parallel_for2d(int first, int second,
                    std::function<void(int, int)> callback);
void parallel_for3d(int first, int second, int third,
                    std::function<void(int, int, int)> callback);
void parallel_for4d(int first, int second, int third, int fourth, std::function<void(int, int, int, int)> callback);
void parallel_for(int first, std::function<void(int)> callback) {
    for (int f = 0; f < first; ++f) {
        callback(f);
    }
}
void parallel_for2d(int first, int second, std::function<void(int, int)> callback) {
    for (int f = 0; f < first; ++f) {
        for (int s = 0; s < second; ++s) {
            callback(f, s);
        }
    }
}
void parallel_for3d(int first, int second, int third, std::function<void(int, int, int)> callback) {
    for (int f = 0; f < first; ++f) {
        for (int s = 0; s < second; ++s) {
            for (int t = 0; t < third; ++t) {
                callback(f, s, t);
            }
        }
    }
}
void parallel_for4d(int first, int second, int third, int fourth, std::function<void(int, int, int, int)> callback) {
    for (int f = 0; f < first; ++f) {
        for (int s = 0; s < second; ++s) {
            for (int t = 0; t < third; ++t) {
                for (int ff = 0; ff < fourth; ++ff) {
                    callback(f, s, t, ff);
                }
            }
        }
    }
}
} // namespace seq

struct Indexer4d {
    int dim3_;
    int dim23_;
    int dim123_;

    explicit Indexer4d(int dim0, int dim1, int dim2, int dim3):
            dim3_(dim3), dim23_(dim2 * dim3), dim123_(dim1 * dim2 * dim3) {
        (void)dim0;
    }

    int operator()(int i, int j, int k, int n) const {
        return  i * dim123_ + j * dim23_ + k * dim3_ + n;
    }
};

void refine_anchors(const float* deltas, const float* scores, const float* anchors,
                    float* proposals, const int anchors_num, const int bottom_H,
                    const int bottom_W, const float img_H, const float img_W,
                    const float min_box_H, const float min_box_W,
                    const float max_delta_log_wh,
                    float coordinates_offset) {
    Indexer4d anchor_idx(bottom_H, bottom_W, anchors_num, 4);
    Indexer4d delta_idx(anchors_num, 4, bottom_H, bottom_W);
    Indexer4d score_idx(anchors_num, 1, bottom_H, bottom_W);
    Indexer4d proposal_idx(bottom_H, bottom_W, anchors_num, 6);

    parallel_for2d(bottom_H, bottom_W, [&](int h, int w) {
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
            x0 = std::max<float>(0.0f, std::min<float>(x0, img_W - coordinates_offset));
            auto t = std::min<float>(y0, img_H - coordinates_offset);
            y0 = std::max<float>(0.0f, t);
            x1 = std::max<float>(0.0f, std::min<float>(x1, img_W - coordinates_offset));
            y1 = std::max<float>(0.0f, std::min<float>(y1, img_H - coordinates_offset));

            // recompute new width & height
            const float box_w = x1 - x0 + coordinates_offset;
            const float box_h = y1 - y0 + coordinates_offset;

            const int p_idx = proposal_idx(h, w, anchor, 0);
            proposals[p_idx + 0] = x0;
            proposals[p_idx + 1] = y0;
            proposals[p_idx + 2] = x1;
            proposals[p_idx + 3] = y1;
            proposals[p_idx + 4] = score;
            proposals[p_idx + 5] = (min_box_W <= box_w) * (min_box_H <= box_h) * 1.0;
        }
    });
} // namespace

void refine_anchors_jit(const jit_refine_anchors_kernel& refine_anchors_kernel,
                        const int32_t* refine_anchor_indices,
                        const uint32_t* refine_anchor_masks,
                        const float* deltas, const float* scores, const float* anchors,
                        float* proposals, const int anchors_num, const int bottom_H,
                        const int bottom_W, const float img_H, const float img_W,
                        const float min_box_H, const float min_box_W,
                        const float max_delta_log_wh,
                        float coordinates_offset) {
    const Indexer4d anchor_idx(bottom_H, bottom_W, anchors_num, 4);
    const Indexer4d delta_idx(anchors_num, 4, bottom_H, bottom_W);
    const Indexer4d score_idx(anchors_num, 1, bottom_H, bottom_W);
    const Indexer4d proposal_idx(bottom_H, bottom_W, anchors_num, 6);

    const uint32_t anchor_anchor_offset   =   anchor_idx(0, 0, 1, 0) - anchor_idx(0, 0, 0, 0);
    const uint32_t anchor_idx_offset      =   anchor_idx(0, 0, 0, 1) - anchor_idx(0, 0, 0, 0);
    const uint32_t delta_anchor_offset    =    delta_idx(1, 0, 0, 0) - delta_idx(0, 0, 0, 0);
    const uint32_t delta_idx_offset       =    delta_idx(0, 1, 0, 0) - delta_idx(0, 0, 0, 0);
    const uint32_t score_anchor_offset    =    score_idx(1, 0, 0, 0) - score_idx(0, 0, 0, 0);
    const uint32_t proposal_anchor_offset = proposal_idx(0, 0, 1, 0) - proposal_idx(0, 0, 0, 0);
    const uint32_t proposal_idx_offset    = proposal_idx(0, 0, 0, 1) - proposal_idx(0, 0, 0, 0);

    parallel_for2d(bottom_H, bottom_W, [&](int h, int w) {
        const uint32_t anchor_start_idx = anchor_idx(h, w, 0, 0);
        const uint32_t delta_start_idx = delta_idx(0, 0, h, w);
        const uint32_t score_start_idx = score_idx(0, 0, h, w);
        const uint32_t proposal_start_idx = proposal_idx(h, w, 0, 0);
        refine_anchors_kernel(jit_refine_anchors_call_args{
                deltas, scores, anchors,
                reinterpret_cast<float *>(&proposals[0]),
                h, w,
                anchors_num,
                refine_anchor_indices,
                refine_anchor_masks,
                anchor_start_idx,
                anchor_anchor_offset,
                anchor_idx_offset,
                delta_start_idx,
                delta_anchor_offset,
                delta_idx_offset,
                score_start_idx,
                score_anchor_offset,
                proposal_start_idx,
                proposal_anchor_offset,
                proposal_idx_offset,
                img_H, img_W,
                min_box_H, min_box_W,
                static_cast<const float>(log(1000. / 16.)),
                coordinates_offset
        });
    });
}

void unpack_boxes(const float* p_proposals, float* unpacked_boxes, int* is_dead, int pre_nms_topn) {
    parallel_for(pre_nms_topn, [&](size_t i) {
        unpacked_boxes[0*pre_nms_topn + i] = p_proposals[6*i + 0];
        unpacked_boxes[1*pre_nms_topn + i] = p_proposals[6*i + 1];
        unpacked_boxes[2*pre_nms_topn + i] = p_proposals[6*i + 2];
        unpacked_boxes[3*pre_nms_topn + i] = p_proposals[6*i + 3];
        unpacked_boxes[4*pre_nms_topn + i] = p_proposals[6*i + 4];
        is_dead[i] = (p_proposals[6*i + 5] == 1.0) ? 0 : 1;
    });
}

void fill_output_blobs(const float* proposals, const int* roi_indices,
                       float* rois, float* scores, uint8_t* roi_num,
                       const int num_proposals, const size_t num_rois, const int post_nms_topn,
                       Precision roi_num_type) {
    const float *src_x0 = proposals + 0 * num_proposals;
    const float *src_y0 = proposals + 1 * num_proposals;
    const float *src_x1 = proposals + 2 * num_proposals;
    const float *src_y1 = proposals + 3 * num_proposals;
    const float *src_score = proposals + 4 * num_proposals;

    parallel_for(num_rois, [&](size_t i) {
        int index = roi_indices[i];
        rois[i * 4 + 0] = src_x0[index];
        rois[i * 4 + 1] = src_y0[index];
        rois[i * 4 + 2] = src_x1[index];
        rois[i * 4 + 3] = src_y1[index];
        scores[i] = src_score[index];
    });

    if (roi_num_type == Precision::I32) {
        int32_t num = static_cast<int32_t>(num_rois);
        memcpy(roi_num, &num, sizeof(int32_t));
    } else if (roi_num_type == Precision::I64) {
        int64_t num = static_cast<int64_t>(num_rois);
        memcpy(roi_num, &num, sizeof(int64_t));
    } else {
        IE_THROW() << "Incorrect element type of roi_num!";
    }
}

} // namespace

bool GenerateProposals::isSupportedOperation
            (const std::shared_ptr<const ngraph::Node>& op, std::string& errorMessage) noexcept {
    try {
        if (!ngraph::as_type_ptr<const ngraph::op::v9::GenerateProposals>(op)) {
            errorMessage = "Node is not an instance of the Proposal from the operations set v0.";
            return false;
        }
    } catch (...) {
        return false;
    }
    return true;
}

GenerateProposals::GenerateProposals
        (const std::shared_ptr<ngraph::Node>& op, const dnnl::engine& eng,
                WeightsSharing::Ptr &cache) : Node(op, eng, cache) {
    std::string errorMessage;
    if (!isSupportedOperation(op, errorMessage)) {
        IE_THROW(NotImplemented) << errorMessage;
    }

    auto proposalOp = ngraph::as_type_ptr<const ngraph::op::v9::GenerateProposals>(op);
    auto proposalAttrs = proposalOp->get_attrs();

    min_size_ = proposalAttrs.min_size;
    nms_thresh_ = proposalAttrs.nms_threshold;
    pre_nms_topn_ = proposalAttrs.pre_nms_count;
    post_nms_topn_ = proposalAttrs.post_nms_count;
    coordinates_offset_ = proposalAttrs.normalized ? 0.f : 1.f;

    roi_indices_.resize(post_nms_topn_);
    for (int i = 0; i < 16; ++i) {
        refine_anchor_indices_.push_back(i);
    }
    for (int i = 0; i < 16; ++i) {
        refine_anchor_masks_.push_back(0xFFFFFFFF);
    }
    for (int i = 0; i < 16; ++i) {
        refine_anchor_masks_.push_back(0x0000);
    }

    if (op->output(0).get_element_type() == ov::element::f32) {
        if (mayiuse(x64::avx512_core)) {
            refine_anchors_kernel_.reset(new jit_refine_anchors_kernel_fp32<x64::avx512_core>{
                jit_refine_anchors_conf{}});
        } else if (mayiuse(x64::avx2)) {
            refine_anchors_kernel_.reset(new jit_refine_anchors_kernel_fp32<x64::avx2>{
                jit_refine_anchors_conf{}});
        } else if (mayiuse(x64::sse41)) {
            refine_anchors_kernel_.reset(new jit_refine_anchors_kernel_fp32<x64::sse41>{
                jit_refine_anchors_conf{}});
        }
        if (refine_anchors_kernel_) {
            refine_anchors_kernel_->create_kernel();
        }
    }

    if (op->output(0).get_element_type() == ov::element::f32) {
        jit_nms_conf nms_jcp {post_nms_topn_, nms_thresh_, coordinates_offset_};
        if (mayiuse(x64::avx512_core)) {
            nms_kernel_.reset(new jit_nms_kernel_fp32<x64::avx512_core>{nms_jcp});
        } else if (mayiuse(x64::avx2)) {
            nms_kernel_.reset(new jit_nms_kernel_fp32<x64::avx2>{nms_jcp});
        } else if (mayiuse(x64::sse41)) {
            nms_kernel_.reset(new jit_nms_kernel_fp32<x64::sse41>{nms_jcp});
        }
        if (nms_kernel_) {
            nms_kernel_->create_kernel();
        }
    }
}

void GenerateProposals::initSupportedPrimitiveDescriptors() {
    if (!supportedPrimitiveDescriptors.empty())
        return;

    auto roiNumPrecision = getOriginalOutputPrecisionAtPort(OUTPUT_ROI_NUM);
    addSupportedPrimDesc({{LayoutType::ncsp, Precision::FP32},
                          {LayoutType::ncsp, Precision::FP32},
                          {LayoutType::ncsp, Precision::FP32},
                          {LayoutType::ncsp, Precision::FP32}},
                         {{LayoutType::ncsp, Precision::FP32},
                          {LayoutType::ncsp, Precision::FP32},
                          {LayoutType::ncsp, roiNumPrecision}},
                         impl_desc_type::ref_any);
}

void GenerateProposals::executeDynamicImpl(dnnl::stream strm) {
    execute(strm);
}

void GenerateProposals::execute(dnnl::stream strm) {
    try {
        if (inputShapes.size() != 4 || outputShapes.size() != 3) {
            IE_THROW() << "Incorrect number of input or output edges!";
        }

        size_t anchor_dims_size = 1;
        const auto &anchorDims = getParentEdgeAt(INPUT_ANCHORS)->getMemory().getStaticDims();
        for (size_t i = 0; i < anchorDims.size(); i++) {
            anchor_dims_size *= anchorDims[i];
        }

        size_t deltas_dims_size = 1;
        const auto &deltaDims = getParentEdgeAt(INPUT_DELTAS)->getMemory().getStaticDims();
        for (size_t i = 1; i < deltaDims.size(); i++) {
            deltas_dims_size *= deltaDims[i];
        }
        if (anchor_dims_size != deltas_dims_size)
            IE_THROW() << "'Anchors' blob size for GenerateProposals is incompatible with 'deltas' blob size!";

        size_t score_dims_size = 1;
        const auto &scoreDims = getParentEdgeAt(INPUT_SCORES)->getMemory().getStaticDims();
        for (size_t i = 1; i < scoreDims.size(); i++) {
            score_dims_size *= scoreDims[i];
        }
        if (deltas_dims_size != (4 * score_dims_size))
            IE_THROW() << "'Deltas' blob size for GenerateProposals is incompatible with 'scores' blob size!";

        size_t im_info_dims_size = 1;
        const auto &infoDims = getParentEdgeAt(INPUT_IM_INFO)->getMemory().getStaticDims();
        for (size_t i = 1; i < infoDims.size(); i++) {
            im_info_dims_size *= infoDims[i];
        }

        // Prepare memory
        const float *p_deltas_item  = reinterpret_cast<const float *>(getParentEdgeAt(INPUT_DELTAS)->getMemoryPtr()->GetPtr());
        const float *p_scores_item  = reinterpret_cast<const float *>(getParentEdgeAt(INPUT_SCORES)->getMemoryPtr()->GetPtr());
        [[maybe_unused]]const float *p_anchors_item = reinterpret_cast<const float *>(getParentEdgeAt(INPUT_ANCHORS)->getMemoryPtr()->GetPtr());
        const float *p_img_info_cpu = reinterpret_cast<const float *>(getParentEdgeAt(INPUT_IM_INFO)->getMemoryPtr()->GetPtr());

        const int anchors_num = scoreDims[1];

        // bottom shape: N x (num_anchors) x H x W
        const int bottom_H = deltaDims[2];
        const int bottom_W = deltaDims[3];

        // number of all proposals = num_anchors * H * W
        const int num_proposals = anchors_num * bottom_H * bottom_W;

        // number of top-n proposals before NMS
        const int pre_nms_topn = std::min<int>(num_proposals, pre_nms_topn_);

        // number of final RoIs
        size_t num_rois = 0;

        // enumerate all proposals
        //   num_proposals = num_anchors * H * W
        //   (x1, y1, x2, y2, score) for each proposal
        // NOTE: for bottom, only foreground scores are passed
        struct ProposalBox {
            float x0;
            float y0;
            float x1;
            float y1;
            float score;
            float keep;

            bool compare_float(float x, float y, float epsilon = 0.01f) const {
                return fabs(x - y) < epsilon;
            }

            bool operator==(const ProposalBox &rhs) const {
                return compare_float(x0, rhs.x0, 0.01f) &&
                       compare_float(y0, rhs.y0, 0.01f) &&
                       compare_float(x1, rhs.x1, 0.01f) &&
                       compare_float(y1, rhs.y1, 0.01f) &&
                       compare_float(score, rhs.score, 0.01f) &&
                       compare_float(keep, rhs.keep, 0.01f);
            }

            bool operator!=(const ProposalBox &rhs) const {
                return !(rhs == *this);
            }
        };
        std::vector<ProposalBox> proposals_(num_proposals);
        std::vector<ProposalBox> cpu_proposals_(num_proposals);
        std::vector<float> unpacked_boxes(5 * pre_nms_topn);
        std::vector<int> is_dead(pre_nms_topn);

        // Execute
        size_t batch_size = scoreDims[0];
        size_t total_num_rois = 0;
        std::vector<float> roi_item, score_item;
        std::vector<int64_t> roi_num(batch_size);
        uint8_t* p_roi_num = reinterpret_cast<uint8_t*>(&roi_num[0]);
        auto roi_num_type = getOriginalOutputPrecisionAtPort(OUTPUT_ROI_NUM);
        const auto roi_num_item_size = roi_num_type == Precision::I32 ? sizeof(int32_t) : sizeof(int64_t);
        for (size_t n = 0; n < batch_size; ++n) {
            // input image height & width
            [[maybe_unused]]const float img_H = p_img_info_cpu[0];
            [[maybe_unused]]const float img_W = p_img_info_cpu[1];
            // scale factor for height & width
            float scale_h = 1.0;
            float scale_w = 1.0;
            if (im_info_dims_size == 3) {
                scale_h = p_img_info_cpu[2];
                scale_w = p_img_info_cpu[2];
            } else if (im_info_dims_size == 4) {
                scale_h = p_img_info_cpu[2];
                scale_w = p_img_info_cpu[3];
            }
            // minimum box width & height
            const float min_box_H = min_size_ * scale_h;
            const float min_box_W = min_size_ * scale_w;

//            refine_anchors_kernel_.reset();
            if (refine_anchors_kernel_) {
                refine_anchors_jit(
                        *refine_anchors_kernel_,
                        refine_anchor_indices_.data(),
                        refine_anchor_masks_.data(),
                        p_deltas_item, p_scores_item, p_anchors_item,
                        reinterpret_cast<float *>(&proposals_[0]),
                        anchors_num,
                        bottom_H, bottom_W,
                        img_H, img_W,
                        min_box_H, min_box_W,
                        static_cast<const float>(log(1000. / 16.)),
                        coordinates_offset_);
            } else {
                refine_anchors(
                    p_deltas_item, p_scores_item, p_anchors_item,
                    reinterpret_cast<float *>(&proposals_[0]),
                    anchors_num,
                    bottom_H, bottom_W,
                    img_H, img_W,
                    min_box_H, min_box_W,
                    static_cast<const float>(log(1000. / 16.)),
                    coordinates_offset_);
            }

            std::partial_sort(proposals_.begin(), proposals_.begin() + pre_nms_topn, proposals_.end(),
                              [](const ProposalBox &struct1, const ProposalBox &struct2) {
                                  return (struct1.score > struct2.score);
                              });

            unpack_boxes(reinterpret_cast<float *>(&proposals_[0]), &unpacked_boxes[0], &is_dead[0], pre_nms_topn);

#ifdef __GNUC__
            if (__builtin_expect(static_cast<bool>(nms_kernel_), true)) {
#else
            if (nms_kernel_) {
#endif
                jit_uni_nms_proposal_kernel::jit_nms_call_args args {
                    pre_nms_topn,
                    is_dead.data(),
                    unpacked_boxes.data(),
                    &unpacked_boxes[2 * pre_nms_topn],
                    &unpacked_boxes[pre_nms_topn],
                    &unpacked_boxes[3 * pre_nms_topn],
                    roi_indices_.data(),
                    &num_rois
                };
                nms_kernel_->operator()(&args);
            } else {
                nms_cpu(pre_nms_topn, &is_dead[0], &unpacked_boxes[0], &roi_indices_[0], &num_rois, 0,
                    nms_thresh_, post_nms_topn_, coordinates_offset_);
            }

            nms_kernel_.reset();
            if (nms_kernel_) {
                int new_num_rois = num_rois;
                (*nms_kernel_)(jit_nms_call_args {
                    pre_nms_topn,
                    is_dead.data(),
                    unpacked_boxes.data(),
                    &unpacked_boxes[2 * pre_nms_topn],
                    &unpacked_boxes[pre_nms_topn],
                    &unpacked_boxes[3 * pre_nms_topn],
                    roi_indices_.data(),
                    &new_num_rois
                });
                num_rois = new_num_rois;
            } else {
                nms_cpu(pre_nms_topn,
                        &is_dead[0],
                        &unpacked_boxes[0],
                        &roi_indices_[0],
                        &num_rois,
                        0,
                        nms_thresh_,
                        post_nms_topn_,
                        coordinates_offset_);
            }

            const size_t new_num_rois = total_num_rois + num_rois;
            roi_item.resize(new_num_rois * 4);
            score_item.resize(new_num_rois);

            fill_output_blobs(&unpacked_boxes[0], &roi_indices_[0], &roi_item[total_num_rois * 4], &score_item[total_num_rois],
                              p_roi_num, pre_nms_topn, num_rois, post_nms_topn_, roi_num_type);
            p_deltas_item += deltas_dims_size;
            p_scores_item += score_dims_size;
            p_img_info_cpu += im_info_dims_size;
            total_num_rois = new_num_rois;
            p_roi_num += roi_num_item_size;
        }
        // copy to out memory
        redefineOutputMemory({VectorDims{total_num_rois, 4}, VectorDims{total_num_rois}, VectorDims{batch_size}});
        float *p_roi_item       = reinterpret_cast<float *>(getChildEdgesAtPort(OUTPUT_ROIS)[0]->getMemoryPtr()->GetPtr());
        float *p_roi_score_item = reinterpret_cast<float *>(getChildEdgesAtPort(OUTPUT_SCORES)[0]->getMemoryPtr()->GetPtr());
        uint8_t* p_roi_num_item = reinterpret_cast<uint8_t *>(getChildEdgesAtPort(OUTPUT_ROI_NUM)[0]->getMemoryPtr()->GetPtr());
        memcpy(p_roi_item, &roi_item[0], roi_item.size() * sizeof(float));
        memcpy(p_roi_score_item, &score_item[0], score_item.size() * sizeof(float));
        memcpy(p_roi_num_item, &roi_num[0], getChildEdgesAtPort(OUTPUT_ROI_NUM)[0]->getMemoryPtr()->GetSize());
    } catch (const std::exception &e) {
        std::string errorMsg = e.what();
        IE_THROW() << errorMsg;
    }
}

bool GenerateProposals::created() const {
    return getType() == Type::GenerateProposals;
}

bool GenerateProposals::needShapeInfer() const {
    return false;
}

bool GenerateProposals::needPrepareParams() const {
    return false;
}

void GenerateProposals::createPrimitive() {
    jit_uni_nms_proposal_kernel::jit_nms_conf jcp { post_nms_topn_, nms_thresh_,
        coordinates_offset_ };
    std::unique_ptr<jit_uni_nms_proposal_kernel> nms_kernel;
    if (mayiuse(avx512_core)) {
        nms_kernel.reset(new jit_uni_nms_proposal_kernel_impl<avx512_core> { jcp });
    } else if (mayiuse(x64::avx2)) {
        nms_kernel.reset(new jit_uni_nms_proposal_kernel_impl<x64::avx2> { jcp });
    } else if (mayiuse(sse41)) {
        nms_kernel.reset(new jit_uni_nms_proposal_kernel_impl<sse41> { jcp });
    } else {
        DEBUG_LOG("Unable to create JIT version of GenerateProposals due to unsupported ISA."
            " Non-JIT version of proposal will be executed.");
    }
    if (nms_kernel) {
        nms_kernel->create_ker();
        nms_kernel_ = std::move(nms_kernel);
    }
}

}   // namespace node
}   // namespace intel_cpu
}   // namespace ov
