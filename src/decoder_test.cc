// Copyright 2021 The libgav1 Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/gav1/decoder.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

#include "gtest/gtest.h"
#include "src/decoder_test_data.h"

namespace libgav1 {
namespace {

constexpr uint8_t kFrame1[] = {OBU_TEMPORAL_DELIMITER, OBU_SEQUENCE_HEADER,
                               OBU_FRAME_1};
constexpr uint8_t kFrame1MeanQp = 81;

constexpr uint8_t kFrame2[] = {OBU_TEMPORAL_DELIMITER, OBU_FRAME_2};
constexpr uint8_t kFrame2MeanQp = 81;

constexpr uint8_t kFrame1WithHdrCllAndHdrMdcv[] = {
    OBU_TEMPORAL_DELIMITER, OBU_SEQUENCE_HEADER, OBU_METADATA_HDR_CLL,
    OBU_METADATA_HDR_MDCV, OBU_FRAME_1};

constexpr uint8_t kFrame2WithItutT35[] = {OBU_TEMPORAL_DELIMITER,
                                          OBU_METADATA_ITUT_T35, OBU_FRAME_2};

constexpr uint8_t kFrame2WithMultipleItutT35[] = {
    OBU_TEMPORAL_DELIMITER, OBU_METADATA_ITUT_T35, OBU_METADATA_ITUT_T35,
    OBU_FRAME_2};

constexpr uint8_t kFrameShowExistingWithMetadata[] = {
    OBU_TEMPORAL_DELIMITER, OBU_METADATA_HDR_CLL, OBU_METADATA_HDR_MDCV,
    OBU_METADATA_ITUT_T35, OBU_FRAME_HEADER_SHOW_EXISTING_FRAME_2};

constexpr uint8_t kFrameShowExistingWithoutMetadata[] = {
    OBU_TEMPORAL_DELIMITER, OBU_FRAME_HEADER_SHOW_EXISTING_FRAME_2};

constexpr uint8_t kFrame1WithFilmGrainAndMetadata[] = {
    OBU_TEMPORAL_DELIMITER, OBU_SEQUENCE_HEADER_WITH_FILM_GRAIN,
    OBU_METADATA_HDR_CLL, OBU_METADATA_HDR_MDCV, OBU_FRAME_1_WITH_FILM_GRAIN};

constexpr uint8_t kFrame2WithFilmGrainAndItutT35[] = {
    OBU_TEMPORAL_DELIMITER, OBU_METADATA_ITUT_T35, OBU_FRAME_2_WITH_FILM_GRAIN};

class DecoderTest : public testing::Test {
 public:
  void SetUp() override;
  void IncrementFramesInUse() { ++frames_in_use_; }
  void DecrementFramesInUse() { --frames_in_use_; }
  void SetBufferPrivateData(void* buffer_private_data) {
    buffer_private_data_ = buffer_private_data;
  }
  void SetReleasedInputBuffer(void* released_input_buffer) {
    released_input_buffer_ = released_input_buffer;
  }

 protected:
  std::unique_ptr<Decoder> decoder_;
  int frames_in_use_ = 0;
  void* buffer_private_data_ = nullptr;
  void* released_input_buffer_ = nullptr;
};

struct FrameBufferPrivate {
  uint8_t* data[3];
};

extern "C" {

static Libgav1StatusCode GetFrameBuffer(
    void* callback_private_data, int bitdepth, Libgav1ImageFormat image_format,
    int width, int height, int left_border, int right_border, int top_border,
    int bottom_border, int stride_alignment, Libgav1FrameBuffer* frame_buffer) {
  Libgav1FrameBufferInfo info;
  Libgav1StatusCode status = Libgav1ComputeFrameBufferInfo(
      bitdepth, image_format, width, height, left_border, right_border,
      top_border, bottom_border, stride_alignment, &info);
  if (status != kLibgav1StatusOk) return status;

  std::unique_ptr<FrameBufferPrivate> buffer_private(new (std::nothrow)
                                                         FrameBufferPrivate);
  if (buffer_private == nullptr) return kLibgav1StatusOutOfMemory;

  for (int i = 0; i < 3; ++i) {
    const size_t size = (i == 0) ? info.y_buffer_size : info.uv_buffer_size;
    buffer_private->data[i] = new (std::nothrow) uint8_t[size];
    if (buffer_private->data[i] == nullptr) {
      return kLibgav1StatusOutOfMemory;
    }
  }

  uint8_t* const y_buffer = buffer_private->data[0];
  uint8_t* const u_buffer =
      (info.uv_buffer_size != 0) ? buffer_private->data[1] : nullptr;
  uint8_t* const v_buffer =
      (info.uv_buffer_size != 0) ? buffer_private->data[2] : nullptr;

  status = Libgav1SetFrameBuffer(&info, y_buffer, u_buffer, v_buffer,
                                 buffer_private.release(), frame_buffer);
  if (status != kLibgav1StatusOk) return status;

  auto* const decoder_test = static_cast<DecoderTest*>(callback_private_data);
  decoder_test->IncrementFramesInUse();
  decoder_test->SetBufferPrivateData(frame_buffer->private_data);
  return kLibgav1StatusOk;
}

static void ReleaseFrameBuffer(void* callback_private_data,
                               void* buffer_private_data) {
  auto* buffer_private = static_cast<FrameBufferPrivate*>(buffer_private_data);
  for (auto& data : buffer_private->data) {
    delete[] data;
  }
  delete buffer_private;
  auto* const decoder_test = static_cast<DecoderTest*>(callback_private_data);
  decoder_test->DecrementFramesInUse();
}

static void ReleaseInputBuffer(void* private_data, void* input_buffer) {
  auto* const decoder_test = static_cast<DecoderTest*>(private_data);
  decoder_test->SetReleasedInputBuffer(input_buffer);
}

}  // extern "C"

void DecoderTest::SetUp() {
  decoder_.reset(new (std::nothrow) Decoder());
  ASSERT_NE(decoder_, nullptr);
  DecoderSettings settings = {};
  settings.frame_parallel = false;
  settings.get_frame_buffer = GetFrameBuffer;
  settings.release_frame_buffer = ReleaseFrameBuffer;
  settings.callback_private_data = this;
  settings.release_input_buffer = ReleaseInputBuffer;
  ASSERT_EQ(decoder_->Init(&settings), kStatusOk);
}

TEST_F(DecoderTest, APIFlowForNonFrameParallelMode) {
  StatusCode status;
  const DecoderBuffer* buffer;

  // Enqueue frame1 for decoding.
  status = decoder_->EnqueueFrame(kFrame1, sizeof(kFrame1), 0,
                                  const_cast<uint8_t*>(kFrame1));
  ASSERT_EQ(status, kStatusOk);

  // In non-frame-parallel mode, decoding happens only in the DequeueFrame call.
  // So there should be no frames in use yet.
  EXPECT_EQ(frames_in_use_, 0);

  // Dequeue the output of frame1.
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(released_input_buffer_, &kFrame1);

  // libgav1 has decoded frame1 and is holding a reference to it.
  EXPECT_EQ(frames_in_use_, 1);
  EXPECT_EQ(buffer_private_data_, buffer->buffer_private_data);

  // Enqueue frame2 for decoding.
  status = decoder_->EnqueueFrame(kFrame2, sizeof(kFrame2), 0,
                                  const_cast<uint8_t*>(kFrame2));
  ASSERT_EQ(status, kStatusOk);

  EXPECT_EQ(frames_in_use_, 1);

  // Dequeue the output of frame2.
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(released_input_buffer_, &kFrame2);

  EXPECT_EQ(frames_in_use_, 2);
  EXPECT_EQ(buffer_private_data_, buffer->buffer_private_data);

  // Signal end of stream (method 1). This should ensure that all the references
  // are released.
  status = decoder_->SignalEOS();
  EXPECT_EQ(status, kStatusOk);

  // libgav1 should have released all the reference frames now.
  EXPECT_EQ(frames_in_use_, 0);

  // Now, the decoder is ready to accept a new coded video sequence.

  // Enqueue frame1 for decoding.
  status = decoder_->EnqueueFrame(kFrame1, sizeof(kFrame1), 0,
                                  const_cast<uint8_t*>(kFrame1));
  ASSERT_EQ(status, kStatusOk);

  EXPECT_EQ(frames_in_use_, 0);

  // Dequeue the output of frame1.
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(released_input_buffer_, &kFrame1);

  EXPECT_EQ(frames_in_use_, 1);
  EXPECT_EQ(buffer_private_data_, buffer->buffer_private_data);

  // Enqueue frame2 for decoding.
  status = decoder_->EnqueueFrame(kFrame2, sizeof(kFrame2), 0,
                                  const_cast<uint8_t*>(kFrame2));
  ASSERT_EQ(status, kStatusOk);

  EXPECT_EQ(frames_in_use_, 1);

  // Dequeue the output of frame2.
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(released_input_buffer_, &kFrame2);

  EXPECT_EQ(frames_in_use_, 2);
  EXPECT_EQ(buffer_private_data_, buffer->buffer_private_data);

  // Signal end of stream (method 2). This should ensure that all the references
  // are released.
  decoder_ = nullptr;

  // libgav1 should have released all the frames now.
  EXPECT_EQ(frames_in_use_, 0);
}

TEST_F(DecoderTest, NonFrameParallelModeEnqueueMultipleFramesWithoutDequeuing) {
  StatusCode status;
  const DecoderBuffer* buffer;

  // Enqueue frame1 for decoding.
  status = decoder_->EnqueueFrame(kFrame1, sizeof(kFrame1), 0,
                                  const_cast<uint8_t*>(kFrame1));
  ASSERT_EQ(status, kStatusOk);

  // Until the output of frame1 is dequeued, no other frames can be enqueued.
  status = decoder_->EnqueueFrame(kFrame2, sizeof(kFrame2), 0,
                                  const_cast<uint8_t*>(kFrame2));
  ASSERT_EQ(status, kStatusTryAgain);

  EXPECT_EQ(frames_in_use_, 0);

  // Dequeue the output of frame1.
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(released_input_buffer_, &kFrame1);

  EXPECT_EQ(frames_in_use_, 1);

  // Delete the decoder instance.
  decoder_ = nullptr;

  EXPECT_EQ(frames_in_use_, 0);
}

TEST_F(DecoderTest, NonFrameParallelModeEOSBeforeDequeuingLastFrame) {
  StatusCode status;
  const DecoderBuffer* buffer;

  // Enqueue frame1 for decoding.
  status = decoder_->EnqueueFrame(kFrame1, sizeof(kFrame1), 0,
                                  const_cast<uint8_t*>(kFrame1));
  ASSERT_EQ(status, kStatusOk);

  EXPECT_EQ(frames_in_use_, 0);

  // Dequeue the output of frame1.
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(released_input_buffer_, &kFrame1);

  // Enqueue frame2 for decoding.
  status = decoder_->EnqueueFrame(kFrame2, sizeof(kFrame2), 0,
                                  const_cast<uint8_t*>(kFrame2));
  ASSERT_EQ(status, kStatusOk);

  EXPECT_EQ(frames_in_use_, 1);

  // Signal end of stream before dequeuing the output of frame2.
  status = decoder_->SignalEOS();
  ASSERT_EQ(status, kStatusOk);

  // In this case, the output of the last frame that was enqueued is lost (which
  // is intentional since end of stream was signaled without dequeueing it).
  EXPECT_EQ(frames_in_use_, 0);
}

TEST_F(DecoderTest, NonFrameParallelModeInvalidFrameAfterEOS) {
  StatusCode status;
  const DecoderBuffer* buffer = nullptr;

  // Enqueue frame1 for decoding.
  status = decoder_->EnqueueFrame(kFrame1, sizeof(kFrame1), 0,
                                  const_cast<uint8_t*>(kFrame1));
  ASSERT_EQ(status, kStatusOk);

  EXPECT_EQ(frames_in_use_, 0);

  // Dequeue the output of frame1.
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(released_input_buffer_, &kFrame1);

  EXPECT_EQ(frames_in_use_, 1);

  // Signal end of stream.
  status = decoder_->SignalEOS();
  EXPECT_EQ(status, kStatusOk);

  // libgav1 should have released all the reference frames now.
  EXPECT_EQ(frames_in_use_, 0);

  // Now, the decoder is ready to accept a new coded video sequence. But, we
  // try to enqueue a frame that does not have a sequence header (which is not
  // allowed).

  // Enqueue frame2 for decoding.
  status = decoder_->EnqueueFrame(kFrame2, sizeof(kFrame2), 0,
                                  const_cast<uint8_t*>(kFrame2));
  ASSERT_EQ(status, kStatusOk);

  EXPECT_EQ(frames_in_use_, 0);

  // Dequeue the output of frame2 (this will fail since no sequence header has
  // been seen since the last EOS signal).
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusBitstreamError);
  EXPECT_EQ(released_input_buffer_, &kFrame2);

  EXPECT_EQ(frames_in_use_, 0);
}

TEST_F(DecoderTest, MetadataObu) {
  StatusCode status;
  const DecoderBuffer* buffer;

  // Enqueue frame1 for decoding.
  status = decoder_->EnqueueFrame(
      kFrame1WithHdrCllAndHdrMdcv, sizeof(kFrame1WithHdrCllAndHdrMdcv), 0,
      const_cast<uint8_t*>(kFrame1WithHdrCllAndHdrMdcv));
  ASSERT_EQ(status, kStatusOk);

  // Dequeue the output of frame1.
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->has_hdr_cll, 1);
  EXPECT_EQ(buffer->has_hdr_mdcv, 1);
  EXPECT_EQ(buffer->itut_t35_count, 0);
  EXPECT_EQ(released_input_buffer_, &kFrame1WithHdrCllAndHdrMdcv);

  // libgav1 has decoded frame1 and is holding a reference to it.
  EXPECT_EQ(frames_in_use_, 1);
  EXPECT_EQ(buffer_private_data_, buffer->buffer_private_data);

  // Enqueue frame2 for decoding.
  status =
      decoder_->EnqueueFrame(kFrame2WithItutT35, sizeof(kFrame2WithItutT35), 0,
                             const_cast<uint8_t*>(kFrame2WithItutT35));
  ASSERT_EQ(status, kStatusOk);

  EXPECT_EQ(frames_in_use_, 1);

  // Dequeue the output of frame2.
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->has_hdr_cll, 0);
  EXPECT_EQ(buffer->has_hdr_mdcv, 0);
  EXPECT_EQ(buffer->itut_t35_count, 1);
  EXPECT_NE(buffer->itut_t35[0].payload_bytes, nullptr);
  EXPECT_GT(buffer->itut_t35[0].payload_size, 0);
  EXPECT_EQ(released_input_buffer_, &kFrame2WithItutT35);

  EXPECT_EQ(frames_in_use_, 2);
  EXPECT_EQ(buffer_private_data_, buffer->buffer_private_data);

  status = decoder_->SignalEOS();
  EXPECT_EQ(status, kStatusOk);
  EXPECT_EQ(frames_in_use_, 0);
}

TEST_F(DecoderTest, MetadataObuMultipleItutT35) {
  StatusCode status;
  const DecoderBuffer* buffer;

  // Enqueue frame1 for decoding.
  status = decoder_->EnqueueFrame(
      kFrame1WithHdrCllAndHdrMdcv, sizeof(kFrame1WithHdrCllAndHdrMdcv), 0,
      const_cast<uint8_t*>(kFrame1WithHdrCllAndHdrMdcv));
  ASSERT_EQ(status, kStatusOk);

  // Dequeue the output of frame1.
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->has_hdr_cll, 1);
  EXPECT_EQ(buffer->has_hdr_mdcv, 1);
  EXPECT_EQ(buffer->itut_t35_count, 0);
  EXPECT_EQ(released_input_buffer_, &kFrame1WithHdrCllAndHdrMdcv);

  // libgav1 has decoded frame1 and is holding a reference to it.
  EXPECT_EQ(frames_in_use_, 1);
  EXPECT_EQ(buffer_private_data_, buffer->buffer_private_data);

  // Enqueue frame2 for decoding.
  status = decoder_->EnqueueFrame(
      kFrame2WithMultipleItutT35, sizeof(kFrame2WithMultipleItutT35), 0,
      const_cast<uint8_t*>(kFrame2WithMultipleItutT35));
  ASSERT_EQ(status, kStatusOk);

  EXPECT_EQ(frames_in_use_, 1);

  // Dequeue the output of frame2.
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->has_hdr_cll, 0);
  EXPECT_EQ(buffer->has_hdr_mdcv, 0);
  EXPECT_EQ(buffer->itut_t35_count, 2);
  EXPECT_NE(buffer->itut_t35[0].payload_bytes, nullptr);
  EXPECT_GT(buffer->itut_t35[0].payload_size, 0);
  EXPECT_NE(buffer->itut_t35[1].payload_bytes, nullptr);
  EXPECT_GT(buffer->itut_t35[1].payload_size, 0);
  EXPECT_EQ(released_input_buffer_, &kFrame2WithMultipleItutT35);

  EXPECT_EQ(frames_in_use_, 2);
  EXPECT_EQ(buffer_private_data_, buffer->buffer_private_data);

  status = decoder_->SignalEOS();
  EXPECT_EQ(status, kStatusOk);
  EXPECT_EQ(frames_in_use_, 0);
}

TEST_F(DecoderTest, MetadataObuWithShowExistingFrame) {
  StatusCode status;
  const DecoderBuffer* buffer;

  // Enqueue frame1 for decoding.
  status = decoder_->EnqueueFrame(kFrame1, sizeof(kFrame1), 0,
                                  const_cast<uint8_t*>(kFrame1));
  ASSERT_EQ(status, kStatusOk);
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->has_hdr_cll, 0);
  EXPECT_EQ(buffer->has_hdr_mdcv, 0);
  EXPECT_EQ(buffer->itut_t35_count, 0);

  // Enqueue frame2 (a showable inter frame that refreshes slot 2) for decoding.
  status = decoder_->EnqueueFrame(kFrame2, sizeof(kFrame2), 0,
                                  const_cast<uint8_t*>(kFrame2));
  ASSERT_EQ(status, kStatusOk);
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->has_hdr_cll, 0);
  EXPECT_EQ(buffer->has_hdr_mdcv, 0);
  EXPECT_EQ(buffer->itut_t35_count, 0);

  // Enqueue show_existing_frame=1 with HDR CLL, HDR MDCV, and ITU-T T.35
  // metadata OBUs.
  status = decoder_->EnqueueFrame(
      kFrameShowExistingWithMetadata, sizeof(kFrameShowExistingWithMetadata), 0,
      const_cast<uint8_t*>(kFrameShowExistingWithMetadata));
  ASSERT_EQ(status, kStatusOk);
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->has_hdr_cll, 1);
  EXPECT_EQ(buffer->has_hdr_mdcv, 1);
  EXPECT_EQ(buffer->itut_t35_count, 1);
  EXPECT_NE(buffer->itut_t35[0].payload_bytes, nullptr);
  EXPECT_GT(buffer->itut_t35[0].payload_size, 0);
  EXPECT_EQ(released_input_buffer_, &kFrameShowExistingWithMetadata);

  // Enqueue show_existing_frame=1 again without metadata OBUs; previously
  // attached metadata on the reference frame must not be carried over.
  status = decoder_->EnqueueFrame(
      kFrameShowExistingWithoutMetadata,
      sizeof(kFrameShowExistingWithoutMetadata), 0,
      const_cast<uint8_t*>(kFrameShowExistingWithoutMetadata));
  ASSERT_EQ(status, kStatusOk);
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->has_hdr_cll, 0);
  EXPECT_EQ(buffer->has_hdr_mdcv, 0);
  EXPECT_EQ(buffer->itut_t35_count, 0);
  EXPECT_EQ(released_input_buffer_, &kFrameShowExistingWithoutMetadata);

  status = decoder_->SignalEOS();
  EXPECT_EQ(status, kStatusOk);
  EXPECT_EQ(frames_in_use_, 0);

  // Verify metadata preservation when a single temporal unit contains multiple
  // displayable frames queued in |output_frame_queue_|.
  decoder_.reset(new (std::nothrow) Decoder());
  ASSERT_NE(decoder_, nullptr);
  DecoderSettings settings = {};
  settings.output_all_layers = true;
  settings.get_frame_buffer = GetFrameBuffer;
  settings.release_frame_buffer = ReleaseFrameBuffer;
  settings.callback_private_data = this;
  settings.release_input_buffer = ReleaseInputBuffer;
  ASSERT_EQ(decoder_->Init(&settings), kStatusOk);

  std::vector<uint8_t> combined_frames;
  combined_frames.insert(combined_frames.end(),
                         std::begin(kFrame1WithHdrCllAndHdrMdcv),
                         std::end(kFrame1WithHdrCllAndHdrMdcv));
  combined_frames.insert(combined_frames.end(), std::begin(kFrame2WithItutT35),
                         std::end(kFrame2WithItutT35));
  combined_frames.insert(combined_frames.end(),
                         std::begin(kFrameShowExistingWithMetadata),
                         std::end(kFrameShowExistingWithMetadata));
  status =
      decoder_->EnqueueFrame(combined_frames.data(), combined_frames.size(), 0,
                             combined_frames.data());
  ASSERT_EQ(status, kStatusOk);

  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->has_hdr_cll, 1);
  EXPECT_EQ(buffer->has_hdr_mdcv, 1);
  EXPECT_EQ(buffer->itut_t35_count, 0);

  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->has_hdr_cll, 0);
  EXPECT_EQ(buffer->has_hdr_mdcv, 0);
  EXPECT_EQ(buffer->itut_t35_count, 1);

  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->has_hdr_cll, 1);
  EXPECT_EQ(buffer->has_hdr_mdcv, 1);
  EXPECT_EQ(buffer->itut_t35_count, 1);

  status = decoder_->SignalEOS();
  EXPECT_EQ(status, kStatusOk);
  EXPECT_EQ(frames_in_use_, 0);
}

TEST_F(DecoderTest, MetadataObuWithShowExistingFrameParallel) {
  decoder_.reset(new (std::nothrow) Decoder());
  ASSERT_NE(decoder_, nullptr);
  DecoderSettings settings = {};
  settings.frame_parallel = true;
  settings.threads = 8;
  settings.blocking_dequeue = true;
  settings.get_frame_buffer = GetFrameBuffer;
  settings.release_frame_buffer = ReleaseFrameBuffer;
  settings.callback_private_data = this;
  settings.release_input_buffer = ReleaseInputBuffer;
  ASSERT_EQ(decoder_->Init(&settings), kStatusOk);

  StatusCode status;
  const DecoderBuffer* buffer;

  // Enqueue all 4 frames before dequeuing any of them, exercising concurrent
  // OBU parsing of show_existing_frame=1 (with and without metadata) while
  // the underlying reference frame (frame2) is still in flight.
  status = decoder_->EnqueueFrame(
      kFrame1WithHdrCllAndHdrMdcv, sizeof(kFrame1WithHdrCllAndHdrMdcv), 0,
      const_cast<uint8_t*>(kFrame1WithHdrCllAndHdrMdcv));
  ASSERT_EQ(status, kStatusOk);
  status =
      decoder_->EnqueueFrame(kFrame2WithItutT35, sizeof(kFrame2WithItutT35), 0,
                             const_cast<uint8_t*>(kFrame2WithItutT35));
  ASSERT_EQ(status, kStatusOk);
  status = decoder_->EnqueueFrame(
      kFrameShowExistingWithMetadata, sizeof(kFrameShowExistingWithMetadata), 0,
      const_cast<uint8_t*>(kFrameShowExistingWithMetadata));
  ASSERT_EQ(status, kStatusOk);
  status = decoder_->EnqueueFrame(
      kFrameShowExistingWithoutMetadata,
      sizeof(kFrameShowExistingWithoutMetadata), 0,
      const_cast<uint8_t*>(kFrameShowExistingWithoutMetadata));
  ASSERT_EQ(status, kStatusOk);

  // Dequeue frame1 (HDR CLL + HDR MDCV).
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->has_hdr_cll, 1);
  EXPECT_EQ(buffer->has_hdr_mdcv, 1);
  EXPECT_EQ(buffer->itut_t35_count, 0);

  // Dequeue frame2 (ITU-T T.35 must still be intact despite subsequent
  // show_existing_frame=1 frames having been parsed concurrently).
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->has_hdr_cll, 0);
  EXPECT_EQ(buffer->has_hdr_mdcv, 0);
  EXPECT_EQ(buffer->itut_t35_count, 1);
  EXPECT_NE(buffer->itut_t35[0].payload_bytes, nullptr);
  EXPECT_GT(buffer->itut_t35[0].payload_size, 0);

  // Dequeue show_existing_frame=1 with metadata (HDR CLL + HDR MDCV + ITU-T
  // T.35).
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->has_hdr_cll, 1);
  EXPECT_EQ(buffer->has_hdr_mdcv, 1);
  EXPECT_EQ(buffer->itut_t35_count, 1);
  EXPECT_NE(buffer->itut_t35[0].payload_bytes, nullptr);
  EXPECT_GT(buffer->itut_t35[0].payload_size, 0);

  // Dequeue show_existing_frame=1 without metadata (no metadata).
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->has_hdr_cll, 0);
  EXPECT_EQ(buffer->has_hdr_mdcv, 0);
  EXPECT_EQ(buffer->itut_t35_count, 0);

  status = decoder_->SignalEOS();
  EXPECT_EQ(status, kStatusOk);
  EXPECT_EQ(frames_in_use_, 0);
}

TEST_F(DecoderTest, MetadataObuWithFilmGrain) {
  StatusCode status;
  const DecoderBuffer* buffer;

  // Enqueue frame1 with film grain (apply_grain=1, refresh_frame_flags=0xff)
  // and HDR CLL + HDR MDCV metadata. ApplyFilmGrain allocates a new output
  // frame buffer; metadata must be preserved on the dequeued buffer.
  status = decoder_->EnqueueFrame(
      kFrame1WithFilmGrainAndMetadata, sizeof(kFrame1WithFilmGrainAndMetadata),
      0, const_cast<uint8_t*>(kFrame1WithFilmGrainAndMetadata));
  ASSERT_EQ(status, kStatusOk);
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->has_hdr_cll, 1);
  EXPECT_EQ(buffer->has_hdr_mdcv, 1);
  EXPECT_EQ(buffer->itut_t35_count, 0);

  // Enqueue frame2 with film grain (apply_grain=1, refresh_frame_flags=0x0c)
  // and ITU-T T.35 metadata.
  status = decoder_->EnqueueFrame(
      kFrame2WithFilmGrainAndItutT35, sizeof(kFrame2WithFilmGrainAndItutT35), 0,
      const_cast<uint8_t*>(kFrame2WithFilmGrainAndItutT35));
  ASSERT_EQ(status, kStatusOk);
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->has_hdr_cll, 0);
  EXPECT_EQ(buffer->has_hdr_mdcv, 0);
  EXPECT_EQ(buffer->itut_t35_count, 1);
  EXPECT_NE(buffer->itut_t35[0].payload_bytes, nullptr);
  EXPECT_GT(buffer->itut_t35[0].payload_size, 0);

  // Enqueue show_existing_frame=1 (referencing frame2, which has
  // apply_grain=1) with HDR CLL, HDR MDCV, and ITU-T T.35 metadata.
  status = decoder_->EnqueueFrame(
      kFrameShowExistingWithMetadata, sizeof(kFrameShowExistingWithMetadata), 0,
      const_cast<uint8_t*>(kFrameShowExistingWithMetadata));
  ASSERT_EQ(status, kStatusOk);
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->has_hdr_cll, 1);
  EXPECT_EQ(buffer->has_hdr_mdcv, 1);
  EXPECT_EQ(buffer->itut_t35_count, 1);
  EXPECT_NE(buffer->itut_t35[0].payload_bytes, nullptr);
  EXPECT_GT(buffer->itut_t35[0].payload_size, 0);

  // Enqueue show_existing_frame=1 again without metadata OBUs.
  status = decoder_->EnqueueFrame(
      kFrameShowExistingWithoutMetadata,
      sizeof(kFrameShowExistingWithoutMetadata), 0,
      const_cast<uint8_t*>(kFrameShowExistingWithoutMetadata));
  ASSERT_EQ(status, kStatusOk);
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(buffer->has_hdr_cll, 0);
  EXPECT_EQ(buffer->has_hdr_mdcv, 0);
  EXPECT_EQ(buffer->itut_t35_count, 0);

  status = decoder_->SignalEOS();
  EXPECT_EQ(status, kStatusOk);
  EXPECT_EQ(frames_in_use_, 0);
}

class ParseOnlyTest : public testing::Test {
 public:
  void SetUp() override;

 protected:
  std::unique_ptr<Decoder> decoder_;
};

void ParseOnlyTest::SetUp() {
  decoder_.reset(new (std::nothrow) Decoder());
  ASSERT_NE(decoder_, nullptr);
  DecoderSettings settings = {};
  settings.parse_only = true;  // parse_only mode activated
  ASSERT_EQ(decoder_->Init(&settings), kStatusOk);
}

TEST_F(ParseOnlyTest, NonFrameParallelModeParseOnly) {
  StatusCode status;
  const DecoderBuffer* buffer;

  // Enqueue frame1 for decoding.
  status = decoder_->EnqueueFrame(kFrame1, sizeof(kFrame1), 0,
                                  const_cast<uint8_t*>(kFrame1));
  ASSERT_EQ(status, kStatusOk);

  // Dequeue the output of frame1.
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_EQ(buffer,
            nullptr);  // in the parse only case the buffer should be nullptr

  // Frame 1 has 1 coding block and the value of it is kFrame1MeanQp.
  std::vector<int> frame1_qp = decoder_->GetFramesMeanQpInTemporalUnit();
  EXPECT_EQ(frame1_qp[0], kFrame1MeanQp);

  // Enqueue frame2 for decoding.
  status = decoder_->EnqueueFrame(kFrame2, sizeof(kFrame2), 0,
                                  const_cast<uint8_t*>(kFrame2));
  ASSERT_EQ(status, kStatusOk);

  // Dequeue the output of frame2.
  status = decoder_->DequeueFrame(&buffer);
  ASSERT_EQ(status, kStatusOk);
  ASSERT_EQ(buffer,
            nullptr);  // in the parse only case the buffer should be nullptr

  // Frame 2 has 4 coding blocks and the mean value of them is kFrame2MeanQp.
  std::vector<int> frame2_qp = decoder_->GetFramesMeanQpInTemporalUnit();
  EXPECT_EQ(frame2_qp[0], kFrame2MeanQp);
}

}  // namespace
}  // namespace libgav1
