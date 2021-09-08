/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#include "channels.h"
#include <math.h>

namespace android::audio_utils::channels {

/**
 * ChannelMix
 *
 * Converts audio streams with different positional channel configurations.
 * Currently only downmix to stereo is supported, so there is no outputChannelMask argument.
 *
 * TODO: Consider conversion to 7.1 and 5.1.
 */
class ChannelMix {
public:

    /**
     * Creates a ChannelMix object
     *
     * Note: If construction is unsuccessful then getInputChannelMask will return
     * AUDIO_CHANNEL_NONE.
     *
     * \param inputChannelMask   channel position mask for input audio data.
     */
    explicit ChannelMix(audio_channel_mask_t inputChannelMask) {
        setInputChannelMask(inputChannelMask);
    }

    ChannelMix() = default;

    /**
     * Set the input channel mask.
     *
     * \param inputChannelMask channel position mask for input data.
     *
     * \return false if the channel mask is not supported.
     */
    bool setInputChannelMask(audio_channel_mask_t inputChannelMask) {
        if (mInputChannelMask != inputChannelMask) {
            if (inputChannelMask & ~((1 << FCC_24) - 1)) {
                return false;  // not channel position mask, or has unknown channels.
            }

            // Compute at what index each channel is: samples will be in the following order:
            //   FL  FR  FC    LFE   BL  BR  BC    SL  SR
            //
            //  (transfer matrix)
            //   FL  FR  FC    LFE   BL  BR  BC    SL  SR
            //   0.5     0.353 0.353 0.5     0.353 0.5
            //       0.5 0.353 0.353     0.5 0.353     0.5
            int index = 0;
            for (unsigned tmp = inputChannelMask; tmp != 0; ++index) {
                const unsigned lowestBit = tmp & -(signed)tmp;
                switch (lowestBit) {
                    case AUDIO_CHANNEL_OUT_FRONT_LEFT:
                    case AUDIO_CHANNEL_OUT_SIDE_LEFT:
                    case AUDIO_CHANNEL_OUT_BACK_LEFT:
                        mMatrix[index][0] = 0.5f;
                        mMatrix[index][1] = 0.f;
                        mLastValidChannelIndexPlusOne = index + 1;
                        break;
                    case AUDIO_CHANNEL_OUT_FRONT_RIGHT:
                    case AUDIO_CHANNEL_OUT_SIDE_RIGHT:
                    case AUDIO_CHANNEL_OUT_BACK_RIGHT:
                        mMatrix[index][0] = 0.f;
                        mMatrix[index][1] = 0.5f;
                        mLastValidChannelIndexPlusOne = index + 1;
                        break;
                    case AUDIO_CHANNEL_OUT_FRONT_CENTER:
                    case AUDIO_CHANNEL_OUT_LOW_FREQUENCY:
                    case AUDIO_CHANNEL_OUT_BACK_CENTER:
                        mMatrix[index][0] = mMatrix[index][1] = 0.5f * MINUS_3_DB_IN_FLOAT;
                        mLastValidChannelIndexPlusOne = index + 1;
                        break;
                    default:
                        mMatrix[index][0] = mMatrix[index][1] = 0.f;
                        break;
                }
                tmp ^= lowestBit;
            }
            mInputChannelMask = inputChannelMask;
            mInputChannelCount = index;
        }
        return true;
    }

    /**
     * Returns the input channel mask.
     */
    audio_channel_mask_t getInputChannelMask() const {
        return mInputChannelMask;
    }

    /**
     * Downmixes audio data in src to dst.
     *
     * \param src          input audio buffer to downmix
     * \param dst          downmixed stereo audio samples
     * \param frameCount   number of frames to downmix
     * \param accumulate   is true if the downmix is added to the destination or
     *                     false if the downmix replaces the destination.
     *
     * \return false if the channel mask set is not supported.
     */
    bool process(const float *src, float *dst, size_t frameCount, bool accumulate) const {
        return accumulate ? processSwitch<true>(src, dst, frameCount)
                : processSwitch<false>(src, dst, frameCount);
    }

    /**
     * Downmixes audio data in src to dst.
     *
     * \param src          input audio buffer to downmix
     * \param dst          downmixed stereo audio samples
     * \param frameCount   number of frames to downmix
     * \param accumulate   is true if the downmix is added to the destination or
     *                     false if the downmix replaces the destination.
     * \param inputChannelMask channel position mask for input data.
     *
     * \return false if the channel mask set is not supported.
     */
    bool process(const float *src, float *dst, size_t frameCount, bool accumulate,
            audio_channel_mask_t inputChannelMask) {
        return setInputChannelMask(inputChannelMask) && process(src, dst, frameCount, accumulate);
    }

private:
    // These values are modified only when the input channel mask changes.
    // Keep alignment for matrix for more stable benchmarking.
    alignas(128) float mMatrix[FCC_24][FCC_2];  // currently only stereo output supported
    audio_channel_mask_t mInputChannelMask = AUDIO_CHANNEL_NONE;
    size_t mLastValidChannelIndexPlusOne = 0;
    size_t mInputChannelCount = 0;

    // Static/const parameters.
    static inline constexpr size_t mOutputChannelCount = FCC_2;    // stereo out only
    static inline constexpr float MINUS_3_DB_IN_FLOAT = M_SQRT1_2; // -3dB = 0.70710678
    static inline constexpr float LIMIT_AMPLITUDE = 1.f;           // 0dB.
    static inline float clamp(float value) {
        return fmin(fmax(value, -LIMIT_AMPLITUDE), LIMIT_AMPLITUDE);
    }

    /**
     * Downmixes audio data in src to dst.
     *
     * ACCUMULATE is true if the downmix is added to the destination or
     *               false if the downmix replaces the destination.
     *
     * \param src          multichannel audio buffer to downmix
     * \param dst          downmixed stereo audio samples
     * \param frameCount   number of multichannel frames to downmix
     *
     * \return false if the CHANNEL_COUNT is not supported.
     */
    template <bool ACCUMULATE>
    bool processSwitch(const float *src, float *dst, size_t frameCount) const {
        constexpr bool ANDROID_SPECIFIC = true;  // change for testing.
        if constexpr (ANDROID_SPECIFIC) {
            switch (mInputChannelMask) {
            case AUDIO_CHANNEL_OUT_QUAD_BACK:
            case AUDIO_CHANNEL_OUT_QUAD_SIDE:
                return specificProcess<4 /* CHANNEL_COUNT */, ACCUMULATE>(src, dst, frameCount);
            case AUDIO_CHANNEL_OUT_5POINT1_BACK:
            case AUDIO_CHANNEL_OUT_5POINT1_SIDE:
                return specificProcess<6 /* CHANNEL_COUNT */, ACCUMULATE>(src, dst, frameCount);
            case AUDIO_CHANNEL_OUT_7POINT1:
                return specificProcess<8 /* CHANNEL_COUNT */, ACCUMULATE>(src, dst, frameCount);
            default:
                break; // handled below.
            }
        }
        return matrixProcess(src, dst, frameCount, ACCUMULATE);
    }

    /**
     * Converts a source audio stream to destination audio stream with a matrix
     * channel conversion.
     *
     * \param src          multichannel audio buffer to downmix
     * \param dst          downmixed stereo audio samples
     * \param frameCount   number of multichannel frames to downmix
     * \param accumulate   is true if the downmix is added to the destination or
     *                     false if the downmix replaces the destination.
     *
     * \return false if the CHANNEL_COUNT is not supported.
     */
    bool matrixProcess(const float *src, float *dst, size_t frameCount, bool accumulate) const {
        // matrix multiply
        if (mInputChannelMask == AUDIO_CHANNEL_NONE) return false;
        while (frameCount) {
            float ch[2]{}; // left, right
            for (size_t i = 0; i < mLastValidChannelIndexPlusOne; ++i) {
                ch[0] += mMatrix[i][0] * src[i];
                ch[1] += mMatrix[i][1] * src[i];
            }
            if (accumulate) {
                ch[0] += dst[0];
                ch[1] += dst[1];
            }
            dst[0] = clamp(ch[0]);
            dst[1] = clamp(ch[1]);
            src += mInputChannelCount;
            dst += mOutputChannelCount;
            --frameCount;
        }
        return true;
    }

    /**
     * Downmixes to stereo a multichannel signal of specified number of channels
     *
     * CHANNEL_COUNT is the number of channels of the src input.
     * ACCUMULATE is true if the downmix is added to the destination or
     *               false if the downmix replaces the destination.
     *
     * \param src          multichannel audio buffer to downmix
     * \param dst          downmixed stereo audio samples
     * \param frameCount   number of multichannel frames to downmix
     *
     * \return false if the CHANNEL_COUNT is not supported.
     */
    template <int CHANNEL_COUNT, bool ACCUMULATE>
    static bool specificProcess(const float *src, float *dst, size_t frameCount) {
        while (frameCount > 0) {
            float ch[2]; // left, right
            if constexpr (CHANNEL_COUNT == 4) { // QUAD
                // sample at index 0 is FL
                // sample at index 1 is FR
                // sample at index 2 is RL (or SL)
                // sample at index 3 is RR (or SR)
                // FL + RL
                ch[0] = src[0] + src[2];
                // FR + RR
                ch[1] = src[1] + src[3];
            } else if constexpr (CHANNEL_COUNT == 6) { // 5.1
                // sample at index 0 is FL
                // sample at index 1 is FR
                // sample at index 2 is FC
                // sample at index 3 is LFE
                // sample at index 4 is RL (or SL)
                // sample at index 5 is RR (or SR)
                const float centerPlusLfeContrib = src[2] + src[3];
                // FL + RL + centerPlusLfeContrib
                ch[0] = src[0] + src[4] + centerPlusLfeContrib * MINUS_3_DB_IN_FLOAT;
                // FR + RR + centerPlusLfeContrib
                ch[1] = src[1] + src[5] + centerPlusLfeContrib * MINUS_3_DB_IN_FLOAT;
            } else if constexpr (CHANNEL_COUNT == 8) { // 7.1
                // sample at index 0 is FL
                // sample at index 1 is FR
                // sample at index 2 is FC
                // sample at index 3 is LFE
                // sample at index 4 is RL
                // sample at index 5 is RR
                // sample at index 6 is SL
                // sample at index 7 is SR
                const float centerPlusLfeContrib = src[2] + src[3];
                // FL + RL + SL + centerPlusLfeContrib
                ch[0] = src[0] + src[4] + src[6] + centerPlusLfeContrib * MINUS_3_DB_IN_FLOAT;
                // FR + RR + SR + centerPlusLfeContrib
                ch[1] = src[1] + src[5] + src[7] + centerPlusLfeContrib * MINUS_3_DB_IN_FLOAT;
            } else {
                return false;
            }
            ch[0] *= 0.5;
            ch[1] *= 0.5;
            if constexpr (ACCUMULATE) {
                dst[0] = clamp(dst[0] + ch[0]);
                dst[1] = clamp(dst[1] + ch[1]);
            } else {
                dst[0] = clamp(ch[0]);
                dst[1] = clamp(ch[1]);
            }
            src += CHANNEL_COUNT;
            dst += mOutputChannelCount;
            --frameCount;
        }
        return true;
    }
};

} // android::audio_utils::channels
