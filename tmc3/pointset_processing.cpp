/* The copyright in this software is being made available under the BSD
 * Licence, included below.  This software may be subject to other third
 * party and contributor rights, including patent rights, and no such
 * rights are granted under this licence.
 *
 * Copyright (c) 2017-2018, ISO/IEC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 *
 * * Neither the name of the ISO/IEC nor the names of its contributors
 *   may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "pointset_processing.h"

#include "colourspace.h"
#include "hls.h"
#include "KDTreeVectorOfVectorsAdaptor.h"

#include <cstddef>
#include <set>
#include <vector>
#include <utility>
#include <map>

namespace pcc {

//============================================================================

template<typename UniqueFn, typename QFn>
SrcMappedPointSet
reducePointSet(const PCCPointSet3& src, UniqueFn uniqueFn, QFn qFn)
{
  SrcMappedPointSet dst;
  int numSrcPoints = src.getPointCount();

  // Build a map of duplicate points
  int numDstPoints = 0;
  if (1) {
    std::map<Vec3<int32_t>, int> qPosToSrcIdx;
    dst.srcIdxDupList.resize(numSrcPoints);
    for (int i = numSrcPoints - 1; i >= 0; i--) {// Starting from the last point in the src 
      // Attempt to insert quantised position
      auto res = qPosToSrcIdx.insert({uniqueFn(src[i]), i});//res->second will be false is key already exists

      // Append to linked list of same positions.
      // Index of the src point (i) or the index of the previous point with
      // the same quantised position
      // res.first->second is either the index of newly inserted point or that of the previous point with same quantized position
      // 0x80000000 = 0b1000 0000 0000 0000 ...(the "mask"), this is used to convert a valid index to negative since only the MSB is 1.
      // If a valid index is inserted, after XOR with 0x80000000, its MSB will be set to 1.

      dst.srcIdxDupList[res.first->second] ^= 0x80000000;
      // If res.first->second is already negative (when key already exists), XOR will revert the value back to positive index (MSB changes back to 0)
      dst.srcIdxDupList[i] = res.first->second | 0x80000000;
      // dst.srcIdxDupList[i], which records the index of point that has the same position as the current point, is assigned by 
      // res.first->second | mask. This will change MSB to 1. 
      res.first->second = i;
      // This line updates the value in the map that contains only unique points. It records the latest index that has this particular position.
      // This line will assign the current i to res.first->second if the current point is new unique point.
    }

    numDstPoints = qPosToSrcIdx.size();// Number of unique points after duplicate removal. (New cloud not generate yet)
  }
  // After the above block, qPosToSrcIdx saves
  // dst.srcIdxDupList saves
  // Number of quantised points is now known
  dst.cloud.resize(numDstPoints);
  dst.idxToSrcIdx.resize(numDstPoints);
  if (src.hasLaserAngles())
    dst.cloud.addLaserAngles();

  //----------------------------------check number of points with dst.srcIdxDupList[i]==i
  int first_dup_count = 0;
  int duplicated_point_count = 0;
  int unique_point_count = 0;
  for (int i = 0; i < numSrcPoints; i++) {
    if (dst.srcIdxDupList[i]==i)
      first_dup_count++;
    if (dst.srcIdxDupList[i] >= 0)
      duplicated_point_count++;
    if (dst.srcIdxDupList[i] < 0)
      unique_point_count++;
  }
  // Debugging purpose: Quantize entire set
  SrcMappedPointSet quantizedSrc;
  quantizedSrc.cloud.resize(numSrcPoints);
  for (int i = 0; i < numSrcPoints; i++) {
    quantizedSrc.cloud[i] = qFn(src[i]);
  }
  // Generate dst outputs
  for (int i = 0, dstIdx = 0; i < numSrcPoints; ++i) {
    // Find head of each linked list
    if (dst.srcIdxDupList[i] >= 0)//Need to make changes to here so that we can add duplicated points as well. 
      continue;

    dst.srcIdxDupList[i] ^= 0x80000000; // 0b100...01 after XORing with 0x80000000 is 1
    dst.idxToSrcIdx[dstIdx] = i;
    if (src.hasLaserAngles() == true)
      dst.cloud.setLaserAngle(dstIdx, src.getLaserAngle(i));
    dst.cloud[dstIdx++] = qFn(src[i]);
  }

  // Add attribute storage to match src
  dst.cloud.addRemoveAttributes(src.hasColors(), src.hasReflectances(), src.hasElongations());

  return dst;
}

// ==============================Tian: Start of custom template function: Selective retention of duplicated points===========================//
template<typename UniqueFn, typename QFn>
SrcMappedPointSet
reducePointSetCustom(const PCCPointSet3& src, UniqueFn uniqueFn, QFn qFn)
{
  SrcMappedPointSet dst;
  int numSrcPoints = src.getPointCount();

  // Build a map of duplicate points
  int numDstPoints = 0;
  if (1) {
    std::map<Vec3<int32_t>, int> qPosToSrcIdx;
    dst.srcIdxDupList.resize(numSrcPoints);
    for (int i = numSrcPoints - 1; i >= 0; i--) {// Starting from the last point in the src 
      // Attempt to insert quantised position
      auto res = qPosToSrcIdx.insert({uniqueFn(src[i]), i});//res->second will be false is key already exists

      // Append to linked list of same positions.
      // Index of the src point (i) or the index of the previous point with
      // the same quantised position
      // res.first->second is either the index of newly inserted point or that of the previous point with same quantized position
      // 0x80000000 = 0b1000 0000 0000 0000 ...(the "mask"), this is used to convert a valid index to negative since only the MSB is 1.
      // If a valid index is inserted, after XOR with 0x80000000, its MSB will be set to 1.

      dst.srcIdxDupList[res.first->second] ^= 0x80000000;
      // If res.first->second is already negative (when key already exists), XOR will revert the value back to positive index (MSB changes back to 0)
      dst.srcIdxDupList[i] = res.first->second | 0x80000000;
      // dst.srcIdxDupList[i], which records the index of point that has the same position as the current point, is assigned by 
      // res.first->second | mask. This will change MSB to 1. 
      res.first->second = i;
      // This line updates the value in the map that contains only unique points. It records the latest index that has this particular position.
      // This line will assign the current i to res.first->second if the current point is new unique point.
    }

    numDstPoints = qPosToSrcIdx.size();// Number of unique points after duplicate removal. (New cloud not generate yet)
  }
  // int numAllowedDup = _gps->custom_duplicate_retention_flag;
  int numAllowedDup = 1000;
  numDstPoints += numAllowedDup;// New custom number of points in dst
  // After the above block, qPosToSrcIdx saves
  // dst.srcIdxDupList saves
  // Number of quantised points is now known
  dst.cloud.resize(numDstPoints);
  dst.idxToSrcIdx.resize(numDstPoints);
  if (src.hasLaserAngles())
    dst.cloud.addLaserAngles();

  // Generate dst outputs
  int numDup = 0;
  for (int i = 0, dstIdx = 0; i < numSrcPoints && dstIdx < numDstPoints; ++i) {
    // Find head of each linked list
    if (dst.srcIdxDupList[i] >= 0) {//Need to make changes to here so that we can add duplicated points as well. 
      if (numDup > numAllowedDup) continue;
      else {
        // dst.srcIdxDupList[i] ^= 0x80000000; // 0b100...01 after XORing with 0x80000000 is 1
        dst.idxToSrcIdx[dstIdx] = i;
        if (src.hasLaserAngles() == true)
          dst.cloud.setLaserAngle(dstIdx, src.getLaserAngle(i));
        dst.cloud[dstIdx++] = qFn(src[i]);
        numDup++;       
      }
    }
    else if (dst.srcIdxDupList[i] < 0) {
      dst.srcIdxDupList[i] ^= 0x80000000; // 0b100...01 after XORing with 0x80000000 is 1
      dst.idxToSrcIdx[dstIdx] = i;
      if (src.hasLaserAngles() == true)
        dst.cloud.setLaserAngle(dstIdx, src.getLaserAngle(i));
      dst.cloud[dstIdx++] = qFn(src[i]);
    }
  }

  // Add attribute storage to match src
  dst.cloud.addRemoveAttributes(src.hasColors(), src.hasReflectances(), src.hasElongations());

  return dst;
}
// ==============================Tian: End of custom template function: Selective retention of duplicated points

//===============================Tian: Start of custom function: Selective retention of duplicated points
SrcMappedPointSet
quantizePositionsCustom(
  const float scaleFactor,
  const Vec3<int> offset,
  const Box3<int> clamp,
  const PCCPointSet3& src)
{
  auto qFn = [=](Vec3<int> point) {
    for (int k = 0; k < 3; k++) {
      double posk = std::round(point[k] * scaleFactor) - offset[k];
      point[k] = PCCClip(int32_t(posk), clamp.min[k], clamp.max[k]);
    }
    return point;
  };

  return reducePointSetCustom(src, qFn, qFn);
}
//===============================Tian: End of custom function: Selective retention of duplicated points

//============================================================================
// Subsample a point cloud, retaining unique points only.
// Uniqueness is assessed by quantising each position by a multiplicative
// @sampleScale.  Output points are quantised by @quantScale with rounding,
// and translated by -@offset.
//
// NB: attributes are not processed.

SrcMappedPointSet
samplePositionsUniq(
  float sampleScale,
  float quantScale,
  Vec3<int> offset,
  const PCCPointSet3& src)
{
  auto diffScale = sampleScale / quantScale;

  return reducePointSet(
    src,
    [=](Vec3<int> point) {
      for (int k = 0; k < 3; k++)
        point[k] = std::round(std::round(point[k] * quantScale) * diffScale);
      return point;
    },
    [=](Vec3<int> point) {
      for (int k = 0; k < 3; k++)
        point[k] = std::round(point[k] * quantScale);
      return point - offset;
    });
}

//============================================================================
// Quantise the geometry of a point cloud, retaining unique points only.
// Points in the @src point cloud are translated by -@offset, quantised by a
// multiplicitive @scaleFactor with rounding, then clamped to @clamp.
//
// NB: attributes are not processed.

SrcMappedPointSet
quantizePositionsUniq(
  const float scaleFactor,
  const Vec3<int> offset,
  const Box3<int> clamp,
  const PCCPointSet3& src)
{
  auto qFn = [=](Vec3<int> point) {
    for (int k = 0; k < 3; k++) {
      double posk = std::round(point[k] * scaleFactor) - offset[k];
      point[k] = PCCClip(int32_t(posk), clamp.min[k], clamp.max[k]);
    }
    return point;
  };

  return reducePointSet(src, qFn, qFn);
}

//============================================================================
// Quantise the geometry of a point cloud, retaining duplicate points.
// Points in the @src point cloud are translated by -@offset, then quantised
// by a multiplicitive @scaleFactor with rounding.
//
// The destination and source point clouds may be the same object.
//
// NB: attributes are preserved

void
quantizePositions(
  const float scaleFactor,
  const Vec3<int> offset,
  const Box3<int> clamp,
  const PCCPointSet3& src,
  PCCPointSet3* dst)
{
  int numSrcPoints = src.getPointCount();

  // In case dst and src point clouds are the same, don't destroy src.
  if (&src != dst) {
    dst->clear();
    dst->addRemoveAttributes(src);
    dst->resize(numSrcPoints);
  }

  for (int i = 0; i < numSrcPoints; ++i) {
    const auto point = src[i];
    auto& dstPoint = (*dst)[i];
    for (int k = 0; k < 3; ++k) {
      double k_pos = std::round(point[k] * scaleFactor) - offset[k];
      dstPoint[k] = PCCClip(int32_t(k_pos), clamp.min[k], clamp.max[k]);
    }
  }

  // don't copy attributes if dst already has them
  if (&src == dst)
    return;

  if (src.hasColors()) {
    for (int i = 0; i < numSrcPoints; ++i)
      dst->setColor(i, src.getColor(i));
  }

  if (src.hasReflectances()) {
    for (int i = 0; i < numSrcPoints; ++i)
      dst->setReflectance(i, src.getReflectance(i));
  }

  if (src.hasElongations()) {
    for (int i = 0; i < numSrcPoints; ++i)
      dst->setElongation(i, src.getElongation(i));
  }

  if (src.hasLaserAngles()) {
    for (int i = 0; i < numSrcPoints; ++i)
      dst->setLaserAngle(i, src.getLaserAngle(i));
  }
}

//============================================================================
// Clamp point co-ordinates in @cloud to @bbox, preserving attributes.

void
clampVolume(Box3<double> bbox, PCCPointSet3* cloud)
{
  int numSrcPoints = cloud->getPointCount();

  for (int i = 0; i < numSrcPoints; ++i) {
    auto& point = (*cloud)[i];
    for (int k = 0; k < 3; ++k)
      point[k] = PCCClip(point[k], bbox.min[k], bbox.max[k]);
  }
}

//============================================================================
// Determine colour attribute values from a reference/source point cloud.
// For each point of the target p_t:
//  - Find the N_1 (1 < N_1) nearest neighbours in source to p_t and create
//    a set of points denoted by Ψ_1.
//  - Find the set of source points that p_t belongs to their set of N_2
//    nearest neighbours. Denote this set of points by Ψ_2.
//  - Compute the distance-weighted average of points in Ψ_1 and Ψ_2 by:
//        \bar{Ψ}_k = ∑_{q∈Ψ_k} c(q)/Δ(q,p_t)
//                    ----------------------- ,
//                    ∑_{q∈Ψ_k} 1/Δ(q,p_t)
//
// where Δ(a,b) denotes the Euclidian distance between the points a and b,
// and c(q) denotes the colour of point q.  Compute the average (or the
// weighted average with the number of points of each set as the weights)
// of \bar{Ψ}̅_1 and \bar{Ψ}̅_2 and transfer it to p_t.
//
// Differences in the scale and translation of the target and source point
// clouds, is handled according to:
//    posInTgt = posInSrc * sourceToTargetScaleFactor - targetToSourceOffset

bool
recolourColour(
  const AttributeDescription& attrDesc,
  const RecolourParams& params,
  const PCCPointSet3& source,
  double sourceToTargetScaleFactor,
  point_t targetToSourceOffset,
  PCCPointSet3& target)
{
  double targetToSourceScaleFactor = 1.0 / sourceToTargetScaleFactor;

  const size_t pointCountSource = source.getPointCount();
  const size_t pointCountTarget = target.getPointCount();
  if (!pointCountSource || !pointCountTarget || !source.hasColors()) {
    return false;
  }

  KDTreeVectorOfVectorsAdaptor<PCCPointSet3, double> kdtreeTarget(
    3, target, 10);
  KDTreeVectorOfVectorsAdaptor<PCCPointSet3, double> kdtreeSource(
    3, source, 10);

  target.addColors();
  std::vector<Vec3<attr_t>> refinedColors1;
  refinedColors1.resize(pointCountTarget);

  Vec3<double> clipMax = double((1 << attrDesc.bitdepth) - 1);

  double maxGeometryDist2Fwd = params.maxGeometryDist2Fwd < 512
    ? params.maxGeometryDist2Fwd
    : std::numeric_limits<double>::max();
  double maxGeometryDist2Bwd = params.maxGeometryDist2Bwd < 512
    ? params.maxGeometryDist2Bwd
    : std::numeric_limits<double>::max();
  double maxAttributeDist2Fwd = params.maxAttributeDist2Fwd < 512
    ? params.maxAttributeDist2Fwd
    : std::numeric_limits<double>::max();
  double maxAttributeDist2Bwd = params.maxAttributeDist2Bwd < 512
    ? params.maxAttributeDist2Bwd
    : std::numeric_limits<double>::max();

  // Forward direction
  const int num_resultsFwd = params.numNeighboursFwd;
  nanoflann::KNNResultSet<double> resultSetFwd(num_resultsFwd);
  std::vector<size_t> indicesFwd(num_resultsFwd);
  std::vector<double> sqrDistFwd(num_resultsFwd);
  for (size_t index = 0; index < pointCountTarget; ++index) {
    resultSetFwd.init(&indicesFwd[0], &sqrDistFwd[0]);

    Vec3<double> posInSrc =
      (target[index] + targetToSourceOffset) * targetToSourceScaleFactor;

    kdtreeSource.index->findNeighbors(
      resultSetFwd, &posInSrc[0], nanoflann::SearchParams(10));

    while (1) {
      if (indicesFwd.size() == 1)
        break;

      if (sqrDistFwd[int(resultSetFwd.size()) - 1] <= maxGeometryDist2Fwd)
        break;

      sqrDistFwd.pop_back();
      indicesFwd.pop_back();
    }

    bool isDone = false;
    if (params.skipAvgIfIdenticalSourcePointPresentFwd) {
      if (sqrDistFwd[0] < 0.0001) {
        refinedColors1[index] = source.getColor(indicesFwd[0]);
        isDone = true;
      }
    }

    if (isDone)
      continue;

    int nNN = indicesFwd.size();
    while (nNN > 0 && !isDone) {
      if (nNN == 1) {
        refinedColors1[index] = source.getColor(indicesFwd[0]);
        isDone = true;
        break;
      }

      std::vector<Vec3<attr_t>> colors;
      colors.resize(0);
      colors.resize(nNN);
      for (int i = 0; i < nNN; ++i) {
        for (int k = 0; k < 3; ++k) {
          colors[i][k] = double(source.getColor(indicesFwd[i])[k]);
        }
      }
      double maxAttributeDist2 = std::numeric_limits<double>::min();
      for (int i = 0; i < nNN; ++i) {
        for (int j = 0; j < nNN; ++j) {
          const double dist2 = (colors[i] - colors[j]).getNorm2<double>();
          if (dist2 > maxAttributeDist2) {
            maxAttributeDist2 = dist2;
          }
        }
      }
      if (maxAttributeDist2 > maxAttributeDist2Fwd) {
        --nNN;
      } else {
        Vec3<double> refinedColor(0.0);
        if (params.useDistWeightedAvgFwd) {
          double sumWeights{0.0};
          for (int i = 0; i < nNN; ++i) {
            const double weight = 1 / (sqrDistFwd[i] + params.distOffsetFwd);
            for (int k = 0; k < 3; ++k) {
              refinedColor[k] += source.getColor(indicesFwd[i])[k] * weight;
            }
            sumWeights += weight;
          }
          refinedColor /= sumWeights;
        } else {
          for (int i = 0; i < nNN; ++i) {
            for (int k = 0; k < 3; ++k) {
              refinedColor[k] += source.getColor(indicesFwd[i])[k];
            }
          }
          refinedColor /= nNN;
        }
        for (int k = 0; k < 3; ++k) {
          refinedColors1[index][k] =
            attr_t(PCCClip(round(refinedColor[k]), 0.0, clipMax[k]));
        }
        isDone = true;
      }
    }
  }

  // Backward direction
  const size_t num_resultsBwd = params.numNeighboursBwd;
  std::vector<size_t> indicesBwd(num_resultsBwd);
  std::vector<double> sqrDistBwd(num_resultsBwd);
  nanoflann::KNNResultSet<double> resultSetBwd(num_resultsBwd);

  struct DistColor {
    double dist;
    Vec3<attr_t> color;
  };
  std::vector<std::vector<DistColor>> refinedColorsDists2;
  refinedColorsDists2.resize(pointCountTarget);

  for (size_t index = 0; index < pointCountSource; ++index) {
    const Vec3<attr_t> color = source.getColor(index);
    resultSetBwd.init(&indicesBwd[0], &sqrDistBwd[0]);

    Vec3<double> posInTgt =
      source[index] * sourceToTargetScaleFactor - targetToSourceOffset;

    kdtreeTarget.index->findNeighbors(
      resultSetBwd, &posInTgt[0], nanoflann::SearchParams(10));

    for (int i = 0; i < num_resultsBwd; ++i) {
      if (sqrDistBwd[i] <= maxGeometryDist2Bwd) {
        refinedColorsDists2[indicesBwd[i]].push_back(
          DistColor{sqrDistBwd[i], color});
      }
    }
  }

  for (size_t index = 0; index < pointCountTarget; ++index) {
    std::sort(
      refinedColorsDists2[index].begin(), refinedColorsDists2[index].end(),
      [](const DistColor& dc1, const DistColor& dc2) {
        return dc1.dist < dc2.dist;
      });
  }

  for (size_t index = 0; index < pointCountTarget; ++index) {
    const Vec3<attr_t> color1 = refinedColors1[index];
    auto& colorsDists2 = refinedColorsDists2[index];
    if (colorsDists2.empty()) {
      target.setColor(index, color1);
      continue;
    }

    bool isDone = false;
    const Vec3<double> centroid1(color1[0], color1[1], color1[2]);
    Vec3<double> centroid2(0.0);
    if (params.skipAvgIfIdenticalSourcePointPresentBwd) {
      if (colorsDists2[0].dist < 0.0001) {
        auto temp = colorsDists2[0];
        colorsDists2.clear();
        colorsDists2.push_back(temp);
        for (int k = 0; k < 3; ++k) {
          centroid2[k] = colorsDists2[0].color[k];
        }
        isDone = true;
      }
    }

    if (!isDone) {
      int nNN = colorsDists2.size();
      while (nNN > 0 && !isDone) {
        nNN = colorsDists2.size();
        if (nNN == 1) {
          auto temp = colorsDists2[0];
          colorsDists2.clear();
          colorsDists2.push_back(temp);
          for (int k = 0; k < 3; ++k) {
            centroid2[k] = colorsDists2[0].color[k];
          }
          isDone = true;
        }
        if (!isDone) {
          std::vector<Vec3<double>> colors;
          colors.resize(0);
          colors.resize(nNN);
          for (int i = 0; i < nNN; ++i) {
            for (int k = 0; k < 3; ++k) {
              colors[i][k] = double(colorsDists2[i].color[k]);
            }
          }
          double maxAttributeDist2 = std::numeric_limits<double>::min();
          for (int i = 0; i < nNN; ++i) {
            for (int j = 0; j < nNN; ++j) {
              const double dist2 = (colors[i] - colors[j]).getNorm2<double>();
              if (dist2 > maxAttributeDist2) {
                maxAttributeDist2 = dist2;
              }
            }
          }
          if (maxAttributeDist2 <= maxAttributeDist2Bwd) {
            for (size_t k = 0; k < 3; ++k) {
              centroid2[k] = 0;
            }
            if (params.useDistWeightedAvgBwd) {
              double sumWeights{0.0};
              for (int i = 0; i < colorsDists2.size(); ++i) {
                const double weight =
                  1 / (sqrt(colorsDists2[i].dist) + params.distOffsetBwd);
                for (size_t k = 0; k < 3; ++k) {
                  centroid2[k] += (colorsDists2[i].color[k] * weight);
                }
                sumWeights += weight;
              }
              centroid2 /= sumWeights;
            } else {
              for (auto& coldist : colorsDists2) {
                for (int k = 0; k < 3; ++k) {
                  centroid2[k] += coldist.color[k];
                }
              }
              centroid2 /= colorsDists2.size();
            }
            isDone = true;
          } else {
            colorsDists2.pop_back();
          }
        }
      }
    }
    double H = double(colorsDists2.size());
    double D2 = 0.0;
    for (const auto color2dist : colorsDists2) {
      auto color2 = color2dist.color;
      for (size_t k = 0; k < 3; ++k) {
        const double d2 = centroid2[k] - color2[k];
        D2 += d2 * d2;
      }
    }
    const double r = double(pointCountTarget) / double(pointCountSource);
    const double delta2 = (centroid2 - centroid1).getNorm2<double>();
    const double eps = 0.000001;

    const bool fixWeight = 1;  // m42538
    if (!(fixWeight || delta2 > eps)) {
      // centroid2 == centroid1
      target.setColor(index, color1);
    } else {
      // centroid2 != centroid1
      double w = 0.0;

      if (!fixWeight) {
        const double alpha = D2 / delta2;
        const double a = H * r - 1.0;
        const double c = alpha * r - 1.0;
        if (fabs(a) < eps) {
          w = -0.5 * c;
        } else {
          const double delta = 1.0 - a * c;
          if (delta >= 0.0) {
            w = (-1.0 + sqrt(delta)) / a;
          }
        }
      }
      const double oneMinusW = 1.0 - w;
      Vec3<double> color0;
      for (size_t k = 0; k < 3; ++k) {
        color0[k] = PCCClip(
          round(w * centroid1[k] + oneMinusW * centroid2[k]), 0.0, clipMax[k]);
      }
      const double rSource = 1.0 / double(pointCountSource);
      const double rTarget = 1.0 / double(pointCountTarget);
      double minError = std::numeric_limits<double>::max();
      Vec3<double> bestColor(color0);
      Vec3<double> color;
      for (int32_t s1 = -params.searchRange; s1 <= params.searchRange; ++s1) {
        color[0] = PCCClip(color0[0] + s1, 0.0, clipMax[0]);
        for (int32_t s2 = -params.searchRange; s2 <= params.searchRange;
             ++s2) {
          color[1] = PCCClip(color0[1] + s2, 0.0, clipMax[1]);
          for (int32_t s3 = -params.searchRange; s3 <= params.searchRange;
               ++s3) {
            color[2] = PCCClip(color0[2] + s3, 0.0, clipMax[2]);

            double e1 = 0.0;
            for (size_t k = 0; k < 3; ++k) {
              const double d = color[k] - color1[k];
              e1 += d * d;
            }
            e1 *= rTarget;

            double e2 = 0.0;
            for (const auto color2dist : colorsDists2) {
              auto color2 = color2dist.color;
              for (size_t k = 0; k < 3; ++k) {
                const double d = color[k] - color2[k];
                e2 += d * d;
              }
            }
            e2 *= rSource;

            const double error = std::max(e1, e2);
            if (error < minError) {
              minError = error;
              bestColor = color;
            }
          }
        }
      }
      target.setColor(
        index,
        Vec3<attr_t>(
          attr_t(bestColor[0]), attr_t(bestColor[1]), attr_t(bestColor[2])));
    }
  }
  return true;
}

//============================================================================
// Determine reflectance attribute values from a reference/source point cloud.
// For each point of the target p_t:
//  - Find the N_1 (1 < N_1) nearest neighbours in source to p_t and create
//    a set of points denoted by Ψ_1.
//  - Find the set of source points that p_t belongs to their set of N_2
//    nearest neighbours. Denote this set of points by Ψ_2.
//  - Compute the distance-weighted average of points in Ψ_1 and Ψ_2 by:
//        \bar{Ψ}_k = ∑_{q∈Ψ_k} c(q)/Δ(q,p_t)
//                    ----------------------- ,
//                    ∑_{q∈Ψ_k} 1/Δ(q,p_t)
//
// where Δ(a,b) denotes the Euclidian distance between the points a and b,
// and c(q) denotes the colour of point q.  Compute the average (or the
// weighted average with the number of points of each set as the weights)
// of \bar{Ψ}̅_1 and \bar{Ψ}̅_2 and transfer it to p_t.
//
// Differences in the scale and translation of the target and source point
// clouds, is handled according to:
//    posInTgt = posInSrc * sourceToTargetScaleFactor - targetToSourceOffset

bool
recolourReflectance(
  const AttributeDescription& attrDesc,
  const RecolourParams& cfg,
  const PCCPointSet3& source,
  double sourceToTargetScaleFactor,
  point_t targetToSourceOffset,
  PCCPointSet3& target)
{
  double targetToSourceScaleFactor = 1.0 / sourceToTargetScaleFactor;

  const size_t pointCountSource = source.getPointCount();
  const size_t pointCountTarget = target.getPointCount();
  if (!pointCountSource || !pointCountTarget || !source.hasReflectances()) {
    return false;
  }
  KDTreeVectorOfVectorsAdaptor<PCCPointSet3, double> kdtreeTarget(
    3, target, 10);
  KDTreeVectorOfVectorsAdaptor<PCCPointSet3, double> kdtreeSource(
    3, source, 10);
  target.addReflectances();
  std::vector<attr_t> refinedReflectances1;
  refinedReflectances1.resize(pointCountTarget);

  double clipMax = (1 << attrDesc.bitdepth) - 1;

  double maxGeometryDist2Fwd = (cfg.maxGeometryDist2Fwd < 512)
    ? cfg.maxGeometryDist2Fwd
    : std::numeric_limits<double>::max();
  double maxGeometryDist2Bwd = (cfg.maxGeometryDist2Bwd < 512)
    ? cfg.maxGeometryDist2Bwd
    : std::numeric_limits<double>::max();
  double maxAttributeDist2Fwd = (cfg.maxAttributeDist2Fwd < 512)
    ? cfg.maxAttributeDist2Fwd
    : std::numeric_limits<double>::max();
  double maxAttributeDist2Bwd = (cfg.maxAttributeDist2Bwd < 512)
    ? cfg.maxAttributeDist2Bwd
    : std::numeric_limits<double>::max();

  // Forward direction
  const int num_resultsFwd = cfg.numNeighboursFwd;
  nanoflann::KNNResultSet<double> resultSetFwd(num_resultsFwd);
  std::vector<size_t> indicesFwd(num_resultsFwd);
  std::vector<double> sqrDistFwd(num_resultsFwd);
  for (size_t index = 0; index < pointCountTarget; ++index) {
    resultSetFwd.init(&indicesFwd[0], &sqrDistFwd[0]);

    Vec3<double> posInSrc =
      (target[index] + targetToSourceOffset) * targetToSourceScaleFactor;

    kdtreeSource.index->findNeighbors(
      resultSetFwd, &posInSrc[0], nanoflann::SearchParams(10));

    while (1) {
      if (indicesFwd.size() == 1)
        break;

      if (sqrDistFwd[int(resultSetFwd.size()) - 1] <= maxGeometryDist2Fwd)
        break;

      sqrDistFwd.pop_back();
      indicesFwd.pop_back();
    }

    bool isDone = false;
    if (cfg.skipAvgIfIdenticalSourcePointPresentFwd) {
      if (sqrDistFwd[0] < 0.0001) {
        refinedReflectances1[index] = source.getReflectance(indicesFwd[0]);
        isDone = true;
      }
    }

    if (isDone)
      continue;

    int nNN = indicesFwd.size();
    while (nNN > 0 && !isDone) {
      if (nNN == 1) {
        refinedReflectances1[index] = source.getReflectance(indicesFwd[0]);
        isDone = true;
        continue;
      }

      std::vector<attr_t> reflectances;
      reflectances.resize(0);
      reflectances.resize(nNN);
      for (int i = 0; i < nNN; ++i) {
        reflectances[i] = double(source.getReflectance(indicesFwd[i]));
      }
      double maxAttributeDist2 = std::numeric_limits<double>::min();
      for (int i = 0; i < nNN; ++i) {
        for (int j = 0; j < nNN; ++j) {
          const double dist2 = pow(reflectances[i] - reflectances[j], 2);
          if (dist2 > maxAttributeDist2)
            maxAttributeDist2 = dist2;
        }
      }
      if (maxAttributeDist2 > maxAttributeDist2Fwd) {
        --nNN;
      } else {
        double refinedReflectance = 0.0;
        if (cfg.useDistWeightedAvgFwd) {
          double sumWeights{0.0};
          for (int i = 0; i < nNN; ++i) {
            const double weight = 1 / (sqrDistFwd[i] + cfg.distOffsetFwd);
            refinedReflectance +=
              source.getReflectance(indicesFwd[i]) * weight;
            sumWeights += weight;
          }
          refinedReflectance /= sumWeights;
        } else {
          for (int i = 0; i < nNN; ++i)
            refinedReflectance += source.getReflectance(indicesFwd[i]);
          refinedReflectance /= nNN;
        }
        refinedReflectances1[index] =
          attr_t(PCCClip(round(refinedReflectance), 0.0, clipMax));
        isDone = true;
      }
    }
  }

  // Backward direction
  const size_t num_resultsBwd = cfg.numNeighboursBwd;
  std::vector<size_t> indicesBwd(num_resultsBwd);
  std::vector<double> sqrDistBwd(num_resultsBwd);
  nanoflann::KNNResultSet<double> resultSetBwd(num_resultsBwd);

  struct DistReflectance {
    double dist;
    attr_t reflectance;
  };
  std::vector<std::vector<DistReflectance>> refinedReflectancesDists2;
  refinedReflectancesDists2.resize(pointCountTarget);

  for (size_t index = 0; index < pointCountSource; ++index) {
    const attr_t reflectance = source.getReflectance(index);
    resultSetBwd.init(&indicesBwd[0], &sqrDistBwd[0]);

    Vec3<double> posInTgt =
      source[index] * sourceToTargetScaleFactor - targetToSourceOffset;

    kdtreeTarget.index->findNeighbors(
      resultSetBwd, &posInTgt[0], nanoflann::SearchParams(10));

    for (int i = 0; i < num_resultsBwd; ++i) {
      if (sqrDistBwd[i] <= maxGeometryDist2Bwd) {
        refinedReflectancesDists2[indicesBwd[i]].push_back(
          DistReflectance{sqrDistBwd[i], reflectance});
      }
    }
  }

  for (size_t index = 0; index < pointCountTarget; ++index) {
    std::sort(
      refinedReflectancesDists2[index].begin(),
      refinedReflectancesDists2[index].end(),
      [](const DistReflectance& dc1, const DistReflectance& dc2) {
        return dc1.dist < dc2.dist;
      });
  }

  for (size_t index = 0; index < pointCountTarget; ++index) {
    const attr_t reflectance1 = refinedReflectances1[index];
    auto& reflectancesDists2 = refinedReflectancesDists2[index];
    if (reflectancesDists2.empty()) {
      target.setReflectance(index, reflectance1);
      continue;
    }

    bool isDone = false;
    const double centroid1 = reflectance1;
    double centroid2 = 0.0;
    if (cfg.skipAvgIfIdenticalSourcePointPresentBwd) {
      if (reflectancesDists2[0].dist < 0.0001) {
        auto temp = reflectancesDists2[0];
        reflectancesDists2.clear();
        reflectancesDists2.push_back(temp);
        centroid2 = reflectancesDists2[0].reflectance;
        isDone = true;
      }
    }
    if (!isDone) {
      int nNN = reflectancesDists2.size();
      while (nNN > 0 && !isDone) {
        nNN = reflectancesDists2.size();
        if (nNN == 1) {
          auto temp = reflectancesDists2[0];
          reflectancesDists2.clear();
          reflectancesDists2.push_back(temp);
          centroid2 = reflectancesDists2[0].reflectance;
          isDone = true;
        }
        if (!isDone) {
          std::vector<double> reflectances;
          reflectances.resize(0);
          reflectances.resize(nNN);
          for (int i = 0; i < nNN; ++i) {
            reflectances[i] = double(reflectancesDists2[i].reflectance);
          }
          double maxAttributeDist2 = std::numeric_limits<double>::min();
          for (int i = 0; i < nNN; ++i) {
            for (int j = 0; j < nNN; ++j) {
              const double dist2 = pow(reflectances[i] - reflectances[j], 2);
              if (dist2 > maxAttributeDist2) {
                maxAttributeDist2 = dist2;
              }
            }
          }
          if (maxAttributeDist2 <= maxAttributeDist2Bwd) {
            centroid2 = 0;
            if (cfg.useDistWeightedAvgBwd) {
              double sumWeights{0.0};
              for (int i = 0; i < reflectancesDists2.size(); ++i) {
                const double weight =
                  1 / (sqrt(reflectancesDists2[i].dist) + cfg.distOffsetBwd);
                centroid2 += (reflectancesDists2[i].reflectance * weight);
                sumWeights += weight;
              }
              centroid2 /= sumWeights;
            } else {
              for (auto& refdist : reflectancesDists2) {
                centroid2 += refdist.reflectance;
              }
              centroid2 /= reflectancesDists2.size();
            }
            isDone = true;
          } else {
            reflectancesDists2.pop_back();
          }
        }
      }
    }
    double H = double(reflectancesDists2.size());
    double D2 = 0.0;
    for (const auto reflectance2dist : reflectancesDists2) {
      auto reflectance2 = reflectance2dist.reflectance;
      const double d2 = centroid2 - reflectance2;
      D2 += d2 * d2;
    }
    const double r = double(pointCountTarget) / double(pointCountSource);
    const double delta2 = pow(centroid2 - centroid1, 2);
    const double eps = 0.000001;

    const bool fixWeight = 1;  // m42538
    if (!(fixWeight || delta2 > eps)) {
      // centroid2 == centroid1
      target.setReflectance(index, reflectance1);
    } else {
      // centroid2 != centroid1
      double w = 0.0;

      if (!fixWeight) {
        const double alpha = D2 / delta2;
        const double a = H * r - 1.0;
        const double c = alpha * r - 1.0;
        if (fabs(a) < eps) {
          w = -0.5 * c;
        } else {
          const double delta = 1.0 - a * c;
          if (delta >= 0.0) {
            w = (-1.0 + sqrt(delta)) / a;
          }
        }
      }
      const double oneMinusW = 1.0 - w;
      double reflectance0;
      reflectance0 =
        PCCClip(round(w * centroid1 + oneMinusW * centroid2), 0.0, clipMax);
      const double rSource = 1.0 / double(pointCountSource);
      const double rTarget = 1.0 / double(pointCountTarget);
      double minError = std::numeric_limits<double>::max();
      double bestReflectance = reflectance0;
      double reflectance;
      for (int32_t s1 = -cfg.searchRange; s1 <= cfg.searchRange; ++s1) {
        reflectance = PCCClip(reflectance0 + s1, 0.0, clipMax);
        double e1 = 0.0;
        const double d = reflectance - reflectance1;
        e1 += d * d;
        e1 *= rTarget;

        double e2 = 0.0;
        for (const auto reflectance2dist : reflectancesDists2) {
          auto reflectance2 = reflectance2dist.reflectance;
          const double d = reflectance - reflectance2;
          e2 += d * d;
        }
        e2 *= rSource;

        const double error = std::max(e1, e2);
        if (error < minError) {
          minError = error;
          bestReflectance = reflectance;
        }
      }
      target.setReflectance(index, attr_t(bestReflectance));
    }
  }
  return true;
}


bool
recolourElongation(
  const AttributeDescription& attrDesc,
  const RecolourParams& cfg,
  const PCCPointSet3& source,
  double sourceToTargetScaleFactor,
  point_t targetToSourceOffset,
  PCCPointSet3& target)
{
  double targetToSourceScaleFactor = 1.0 / sourceToTargetScaleFactor;

  const size_t pointCountSource = source.getPointCount();
  const size_t pointCountTarget = target.getPointCount();
  if (!pointCountSource || !pointCountTarget || !source.hasElongations()) {
    return false;
  }
  KDTreeVectorOfVectorsAdaptor<PCCPointSet3, double> kdtreeTarget(
    3, target, 10);
  KDTreeVectorOfVectorsAdaptor<PCCPointSet3, double> kdtreeSource(
    3, source, 10);
  target.addElongations();
  std::vector<attr_t> refinedElongations1;
  refinedElongations1.resize(pointCountTarget);

  double clipMax = (1 << attrDesc.bitdepth) - 1;

  double maxGeometryDist2Fwd = (cfg.maxGeometryDist2Fwd < 512)
    ? cfg.maxGeometryDist2Fwd
    : std::numeric_limits<double>::max();
  double maxGeometryDist2Bwd = (cfg.maxGeometryDist2Bwd < 512)
    ? cfg.maxGeometryDist2Bwd
    : std::numeric_limits<double>::max();
  double maxAttributeDist2Fwd = (cfg.maxAttributeDist2Fwd < 512)
    ? cfg.maxAttributeDist2Fwd
    : std::numeric_limits<double>::max();
  double maxAttributeDist2Bwd = (cfg.maxAttributeDist2Bwd < 512)
    ? cfg.maxAttributeDist2Bwd
    : std::numeric_limits<double>::max();

  // Forward direction
  const int num_resultsFwd = cfg.numNeighboursFwd;
  nanoflann::KNNResultSet<double> resultSetFwd(num_resultsFwd);
  std::vector<size_t> indicesFwd(num_resultsFwd);
  std::vector<double> sqrDistFwd(num_resultsFwd);
  for (size_t index = 0; index < pointCountTarget; ++index) {
    resultSetFwd.init(&indicesFwd[0], &sqrDistFwd[0]);

    Vec3<double> posInSrc =
      (target[index] + targetToSourceOffset) * targetToSourceScaleFactor;

    kdtreeSource.index->findNeighbors(
      resultSetFwd, &posInSrc[0], nanoflann::SearchParams(10));

    while (1) {
      if (indicesFwd.size() == 1)
        break;

      if (sqrDistFwd[int(resultSetFwd.size()) - 1] <= maxGeometryDist2Fwd)
        break;

      sqrDistFwd.pop_back();
      indicesFwd.pop_back();
    }

    bool isDone = false;
    if (cfg.skipAvgIfIdenticalSourcePointPresentFwd) {
      if (sqrDistFwd[0] < 0.0001) {
        refinedElongations1[index] = source.getElongation(indicesFwd[0]);
        isDone = true;
      }
    }

    if (isDone)
      continue;

    int nNN = indicesFwd.size();
    while (nNN > 0 && !isDone) {
      if (nNN == 1) {
        refinedElongations1[index] = source.getElongation(indicesFwd[0]);
        isDone = true;
        continue;
      }

      std::vector<attr_t> elongations;
      elongations.resize(0);
      elongations.resize(nNN);
      for (int i = 0; i < nNN; ++i) {
        elongations[i] = double(source.getElongation(indicesFwd[i]));
      }
      double maxAttributeDist2 = std::numeric_limits<double>::min();
      for (int i = 0; i < nNN; ++i) {
        for (int j = 0; j < nNN; ++j) {
          const double dist2 = pow(elongations[i] - elongations[j], 2);
          if (dist2 > maxAttributeDist2)
            maxAttributeDist2 = dist2;
        }
      }
      if (maxAttributeDist2 > maxAttributeDist2Fwd) {
        --nNN;
      } else {
        double refinedElongation = 0.0;
        if (cfg.useDistWeightedAvgFwd) {
          double sumWeights{0.0};
          for (int i = 0; i < nNN; ++i) {
            const double weight = 1 / (sqrDistFwd[i] + cfg.distOffsetFwd);
            refinedElongation +=
              source.getElongation(indicesFwd[i]) * weight;
            sumWeights += weight;
          }
          refinedElongation /= sumWeights;
        } else {
          for (int i = 0; i < nNN; ++i)
            refinedElongation += source.getElongation(indicesFwd[i]);
          refinedElongation /= nNN;
        }
        refinedElongations1[index] =
          attr_t(PCCClip(round(refinedElongation), 0.0, clipMax));
        isDone = true;
      }
    }
  }

  // Backward direction
  const size_t num_resultsBwd = cfg.numNeighboursBwd;
  std::vector<size_t> indicesBwd(num_resultsBwd);
  std::vector<double> sqrDistBwd(num_resultsBwd);
  nanoflann::KNNResultSet<double> resultSetBwd(num_resultsBwd);

  struct DistElongation {
    double dist;
    attr_t elongation;
  };
  std::vector<std::vector<DistElongation>> refinedElongationsDists2;
  refinedElongationsDists2.resize(pointCountTarget);

  for (size_t index = 0; index < pointCountSource; ++index) {
    const attr_t elongation = source.getElongation(index);
    resultSetBwd.init(&indicesBwd[0], &sqrDistBwd[0]);

    Vec3<double> posInTgt =
      source[index] * sourceToTargetScaleFactor - targetToSourceOffset;

    kdtreeTarget.index->findNeighbors(
      resultSetBwd, &posInTgt[0], nanoflann::SearchParams(10));

    for (int i = 0; i < num_resultsBwd; ++i) {
      if (sqrDistBwd[i] <= maxGeometryDist2Bwd) {
        refinedElongationsDists2[indicesBwd[i]].push_back(
          DistElongation{sqrDistBwd[i], elongation});
      }
    }
  }

  for (size_t index = 0; index < pointCountTarget; ++index) {
    std::sort(
      refinedElongationsDists2[index].begin(),
      refinedElongationsDists2[index].end(),
      [](const DistElongation& dc1, const DistElongation& dc2) {
        return dc1.dist < dc2.dist;
      });
  }

  for (size_t index = 0; index < pointCountTarget; ++index) {
    const attr_t elongation1 = refinedElongations1[index];
    auto& elongationsDists2 = refinedElongationsDists2[index];
    if (elongationsDists2.empty()) {
      target.setElongation(index, elongation1);
      continue;
    }

    bool isDone = false;
    const double centroid1 = elongation1;
    double centroid2 = 0.0;
    if (cfg.skipAvgIfIdenticalSourcePointPresentBwd) {
      if (elongationsDists2[0].dist < 0.0001) {
        auto temp = elongationsDists2[0];
        elongationsDists2.clear();
        elongationsDists2.push_back(temp);
        centroid2 = elongationsDists2[0].elongation;
        isDone = true;
      }
    }
    if (!isDone) {
      int nNN = elongationsDists2.size();
      while (nNN > 0 && !isDone) {
        nNN = elongationsDists2.size();
        if (nNN == 1) {
          auto temp = elongationsDists2[0];
          elongationsDists2.clear();
          elongationsDists2.push_back(temp);
          centroid2 = elongationsDists2[0].elongation;
          isDone = true;
        }
        if (!isDone) {
          std::vector<double> elongations;
          elongations.resize(0);
          elongations.resize(nNN);
          for (int i = 0; i < nNN; ++i) {
            elongations[i] = double(elongationsDists2[i].elongation);
          }
          double maxAttributeDist2 = std::numeric_limits<double>::min();
          for (int i = 0; i < nNN; ++i) {
            for (int j = 0; j < nNN; ++j) {
              const double dist2 = pow(elongations[i] - elongations[j], 2);
              if (dist2 > maxAttributeDist2) {
                maxAttributeDist2 = dist2;
              }
            }
          }
          if (maxAttributeDist2 <= maxAttributeDist2Bwd) {
            centroid2 = 0;
            if (cfg.useDistWeightedAvgBwd) {
              double sumWeights{0.0};
              for (int i = 0; i < elongationsDists2.size(); ++i) {
                const double weight =
                  1 / (sqrt(elongationsDists2[i].dist) + cfg.distOffsetBwd);
                centroid2 += (elongationsDists2[i].elongation * weight);
                sumWeights += weight;
              }
              centroid2 /= sumWeights;
            } else {
              for (auto& refdist : elongationsDists2) {
                centroid2 += refdist.elongation;
              }
              centroid2 /= elongationsDists2.size();
            }
            isDone = true;
          } else {
            elongationsDists2.pop_back();
          }
        }
      }
    }
    double H = double(elongationsDists2.size());
    double D2 = 0.0;
    for (const auto elongation2dist : elongationsDists2) {
      auto elongation2 = elongation2dist.elongation;
      const double d2 = centroid2 - elongation2;
      D2 += d2 * d2;
    }
    const double r = double(pointCountTarget) / double(pointCountSource);
    const double delta2 = pow(centroid2 - centroid1, 2);
    const double eps = 0.000001;

    const bool fixWeight = 1;  // m42538
    if (!(fixWeight || delta2 > eps)) {
      // centroid2 == centroid1
      target.setElongation(index, elongation1);
    } else {
      // centroid2 != centroid1
      double w = 0.0;

      if (!fixWeight) {
        const double alpha = D2 / delta2;
        const double a = H * r - 1.0;
        const double c = alpha * r - 1.0;
        if (fabs(a) < eps) {
          w = -0.5 * c;
        } else {
          const double delta = 1.0 - a * c;
          if (delta >= 0.0) {
            w = (-1.0 + sqrt(delta)) / a;
          }
        }
      }
      const double oneMinusW = 1.0 - w;
      double elongation0;
      elongation0 =
        PCCClip(round(w * centroid1 + oneMinusW * centroid2), 0.0, clipMax);
      const double rSource = 1.0 / double(pointCountSource);
      const double rTarget = 1.0 / double(pointCountTarget);
      double minError = std::numeric_limits<double>::max();
      double bestElongation = elongation0;
      double elongation;
      for (int32_t s1 = -cfg.searchRange; s1 <= cfg.searchRange; ++s1) {
        elongation = PCCClip(elongation0 + s1, 0.0, clipMax);
        double e1 = 0.0;
        const double d = elongation - elongation1;
        e1 += d * d;
        e1 *= rTarget;

        double e2 = 0.0;
        for (const auto elongation2dist : elongationsDists2) {
          auto elongation2 = elongation2dist.elongation;
          const double d = elongation - elongation2;
          e2 += d * d;
        }
        e2 *= rSource;

        const double error = std::max(e1, e2);
        if (error < minError) {
          minError = error;
          bestElongation = elongation;
        }
      }
      target.setElongation(index, attr_t(bestElongation));
    }
  }
  return true;
}

//============================================================================
// Colour attributes of a target point cloud given a source.
//
// Differences in the scale and translation of the target and source point
// clouds, is handled according to:
//   posInTgt = posInSrc * sourceToTargetScaleFactor - tgtToSrcOffset

int
recolour(
  const AttributeDescription& desc,
  const RecolourParams& cfg,
  const PCCPointSet3& source,
  float sourceToTargetScaleFactor,
  point_t tgtToSrcOffset,
  PCCPointSet3* target)
{
  // todo(df): fix the incorrect assumption here that 3-component
  // attributes are colour (and that single components are reflectance)
  if (desc.attributeLabel == KnownAttributeLabel::kColour) {
    bool ok = recolourColour(
      desc, cfg, source, sourceToTargetScaleFactor, tgtToSrcOffset, *target);

    if (!ok) {
      std::cout << "Error: can't transfer colors!" << std::endl;
      return -1;
    }
  }

  if (desc.attributeLabel == KnownAttributeLabel::kReflectance) {
    bool ok = recolourReflectance(
      desc, cfg, source, sourceToTargetScaleFactor, tgtToSrcOffset, *target);

    if (!ok) {
      std::cout << "Error: can't transfer reflectance!" << std::endl;
      return -1;
    }
  }

  if (desc.attributeLabel == KnownAttributeLabel::kElongation) {
    bool ok = recolourElongation(
      desc, cfg, source, sourceToTargetScaleFactor, tgtToSrcOffset, *target);
    
    if (!ok) {
      std::cout << "Error: can't transfer reflectance!" << std::endl;
      return -1;
    }
  }

  return 0;
}

//============================================================================

void
convertGbrToYCgCoR(int bitDepth, PCCPointSet3& cloud)
{
  for (int i = 0; i < cloud.getPointCount(); i++) {
    auto& val = cloud.getColor(i);
    val = transformGbrToYCgCoR(bitDepth, val);
  }
}

//============================================================================

void
convertYCgCoRToGbr(int bitDepth, PCCPointSet3& cloud)
{
  for (int i = 0; i < cloud.getPointCount(); i++) {
    auto& val = cloud.getColor(i);
    val = transformYCgCoRToGbr(bitDepth, val);
  }
}

//============================================================================

void
convertGbrToYCbCrBt709(PCCPointSet3& cloud)
{
  for (int i = 0; i < cloud.getPointCount(); i++) {
    auto& val = cloud.getColor(i);
    val = transformGbrToYCbCrBt709(val);
  }
}

//============================================================================

void
convertYCbCrBt709ToGbr(PCCPointSet3& cloud)
{
  for (int i = 0; i < cloud.getPointCount(); i++) {
    auto& val = cloud.getColor(i);
    val = transformYCbCrBt709ToGbr(val);
  }
}

//============================================================================
double
roundAtDigit(double x, double digit)
{
  return std::round(x * digit) / digit;
}

//============================================================================

std::vector<int>
orderByAzimuth(
  PCCPointSet3& cloud,
  int start,
  int end,
  double recipBinWidth,
  Vec3<int32_t> origin)
{
  // build a list of inxdexes to sort
  auto pointCount = end - start;
  std::vector<int> order(pointCount);
  for (int i = 0; i < pointCount; i++)
    order[i] = start + i;

  std::sort(order.begin(), order.end(), [&](int aIdx, int bIdx) {
    auto a = cloud[aIdx] - origin;
    auto b = cloud[bIdx] - origin;

    double rA = hypot(a[0], a[1]);
    double phiA = atan2(a[1], a[0]);
    double tanThetaA = a[2] / rA;

    double rB = hypot(b[0], b[1]);
    double phiB = atan2(b[1], b[0]);
    double tanThetaB = b[2] / rB;

    // quantise azimith to specified precision
    if (recipBinWidth != 0.) {
      phiA = std::round(phiA * recipBinWidth);
      phiB = std::round(phiB * recipBinWidth);
    }

    // NB: the a < b comparison adds some stability to the sort.  It is not
    // required in an actual implementation.  Either slightly more performance
    // can be achieved by sorting by a second data dependent dimension, or
    // efficiency can be improved by removing the stability (at a cost of
    // being able to reproduce the exact same bitstream).

    return phiB != phiA ? phiA < phiB
                        : rA != rB ? rA < rB : tanThetaA < tanThetaB;
  });

  return order;
}

//============================================================================
// Sorts according to azimuth.
// \param recipBinWidth is the reciprocal bin width used in sorting.
//        recipBinWidth = 0 disables binning.

void
sortByAzimuth(
  PCCPointSet3& cloud,
  int start,
  int end,
  double recipBinWidth,
  Vec3<int32_t> origin)
{
  auto pointCount = end - start;
  auto order = orderByAzimuth(cloud, start, end, recipBinWidth, origin);

  // inefficiently reorder the point cloud
  for (int i = 0; i < pointCount; i++) {
    while (order[i] - start != i) {
      cloud.swapPoints(order[i], order[order[i] - start]);
      std::swap(order[i], order[order[i] - start]);
    }
  }
}

//============================================================================

std::vector<int>
orderByRadius(PCCPointSet3& cloud, int start, int end, Vec3<int32_t> origin)
{
  // build a list of inxdexes to sort
  auto pointCount = end - start;
  std::vector<int> order(pointCount);
  for (int i = 0; i < pointCount; i++)
    order[i] = start + i;

  std::sort(order.begin(), order.end(), [&](int a, int b) {
    auto aPos = cloud[a] - origin;
    auto bPos = cloud[b] - origin;
    auto aT = aPos[0] * aPos[0] + aPos[1] * aPos[1];
    auto bT = bPos[0] * bPos[0] + bPos[1] * bPos[1];
    // NB: the a < b comparison adds some stability to the sort.  It is not
    // required in an actual implementation.  Either slightly more performance
    // can be achieved by sorting by a second data dependent dimension, or
    // efficiency can be improved by removing the stability (at a cost of
    // being able to reproduce the exact same bitstream).
    return aT != bT ? aT < bT : a < b;
  });

  return order;
}

//============================================================================

void
sortByRadius(PCCPointSet3& cloud, int start, int end, Vec3<int32_t> origin)
{
  auto pointCount = end - start;
  auto order = orderByRadius(cloud, start, end, origin);

  // inefficiently reorder the point cloud
  for (int i = 0; i < pointCount; i++) {
    while (order[i] - start != i) {
      cloud.swapPoints(order[i], order[order[i] - start]);
      std::swap(order[i], order[order[i] - start]);
    }
  }
}

//============================================================================

std::vector<int>
orderByLaserAngle(
  PCCPointSet3& cloud,
  int start,
  int end,
  double recipBinWidth,
  Vec3<int32_t> origin)
{
  // build a list of inxdexes to sort
  auto pointCount = end - start;
  std::vector<int> order(pointCount);
  for (int i = 0; i < pointCount; i++)
    order[i] = start + i;

  std::sort(order.begin(), order.end(), [&](int aIdx, int bIdx) {
    auto a = cloud[aIdx] - origin;
    auto b = cloud[bIdx] - origin;

    double rA = hypot(a[0], a[1]);
    double phiA = cloud.getLaserAngle(aIdx);
    double tanThetaA = a[2] / rA;

    double rB = hypot(b[0], b[1]);
    double phiB = cloud.getLaserAngle(bIdx);
    double tanThetaB = b[2] / rB;

    // quantise azimith to specified precision
    if (recipBinWidth != 0.) {
      phiA = std::round(phiA * recipBinWidth);
      phiB = std::round(phiB * recipBinWidth);
    }

    // NB: the a < b comparison adds some stability to the sort.  It is not
    // required in an actual implementation.  Either slightly more performance
    // can be achieved by sorting by a second data dependent dimension, or
    // efficiency can be improved by removing the stability (at a cost of
    // being able to reproduce the exact same bitstream).

    return phiB != phiA ? phiA < phiB
                        : rA != rB ? rA < rB : tanThetaA < tanThetaB;
  });

  return order;
}

//============================================================================
// Sorts according to azimuth.
// \param recipBinWidth is the reciprocal bin width used in sorting.
//        recipBinWidth = 0 disables binning.

void
sortByLaserAngle(
  PCCPointSet3& cloud,
  int start,
  int end,
  double recipBinWidth,
  Vec3<int32_t> origin)
{
  auto pointCount = end - start;
  std::vector<int> order;
  if (cloud.hasLaserAngles())
    order = orderByLaserAngle(cloud, start, end, recipBinWidth, origin);
  else
    order = orderByAzimuth(cloud, start, end, recipBinWidth, origin);

  // inefficiently reorder the point cloud
  for (int i = 0; i < pointCount; i++) {
    while (order[i] - start != i) {
      cloud.swapPoints(order[i], order[order[i] - start]);
      std::swap(order[i], order[order[i] - start]);
    }
  }
}

}  // namespace pcc
