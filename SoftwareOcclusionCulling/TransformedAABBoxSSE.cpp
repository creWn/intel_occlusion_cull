//--------------------------------------------------------------------------------------
// Copyright 2011 Intel Corporation
// All Rights Reserved
//
// Permission is granted to use, copy, distribute and prepare derivative works of this
// software for any purpose and without fee, provided, that the above copyright notice
// and this statement appear in all copies.  Intel makes no representations about the
// suitability of this software for any purpose.  THIS SOFTWARE IS PROVIDED "AS IS."
// INTEL SPECIFICALLY DISCLAIMS ALL WARRANTIES, EXPRESS OR IMPLIED, AND ALL LIABILITY,
// INCLUDING CONSEQUENTIAL AND OTHER INDIRECT DAMAGES, FOR THE USE OF THIS SOFTWARE,
// INCLUDING LIABILITY FOR INFRINGEMENT OF ANY PROPRIETARY RIGHTS, AND INCLUDING THE
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.  Intel does not
// assume any responsibility for any errors which may appear in this software nor any
// responsibility to update it.
//
//--------------------------------------------------------------------------------------

#include "TransformedAABBoxSSE.h"

static const UINT sBBIndexList[AABB_INDICES] =
{
	// index for top 
	1, 3, 2,
	0, 3, 1,

	// index for bottom
	5, 7, 4,
	6, 7, 5,

	// index for left
	1, 7, 6,
	2, 7, 1,

	// index for right
	3, 5, 4,
	0, 5, 3,

	// index for back
	2, 4, 7,
	3, 4, 2,

	// index for front
	0, 6, 5,
	1, 6, 0,
};

TransformedAABBoxSSE::TransformedAABBoxSSE()
	: mpCPUTModel(NULL),
	  mVisible(NULL),
	  mInsideViewFrustum(true),
	  mOccludeeSizeThreshold(0.0f),
	  mTooSmall(false)
{
	mWorldMatrix = (__m128*)_aligned_malloc(sizeof(float) * 4 * 4, 16);
	mpBBVertexList = (__m128*)_aligned_malloc(sizeof(float) * 4 * AABB_VERTICES, 16);
	mCumulativeMatrix = (__m128*)_aligned_malloc(sizeof(float) * 4 * 4, 16); 
}

TransformedAABBoxSSE::~TransformedAABBoxSSE()
{
	_aligned_free(mWorldMatrix);
	_aligned_free(mpBBVertexList);
	_aligned_free(mCumulativeMatrix);
}

//--------------------------------------------------------------------------
// Get the bounding box center and half vector
// Create the vertex and index list for the triangles that make up the bounding box
//--------------------------------------------------------------------------
void TransformedAABBoxSSE::CreateAABBVertexIndexList(CPUTModelDX11 *pModel)
{
	mpCPUTModel = pModel;
	float* world = (float*)pModel->GetWorldMatrix();

	mWorldMatrix[0] = _mm_loadu_ps(world + 0);
	mWorldMatrix[1] = _mm_loadu_ps(world + 4);
	mWorldMatrix[2] = _mm_loadu_ps(world + 8);
	mWorldMatrix[3] = _mm_loadu_ps(world + 12);

	pModel->GetBoundsObjectSpace(&mBBCenter, &mBBHalf);

	float3 min = mBBCenter - mBBHalf;
	float3 max = mBBCenter + mBBHalf;
	
	//Top 4 vertices in BB
	mpBBVertexList[0] = _mm_set_ps(1.0f, max.z, max.y, max.x);
	mpBBVertexList[1] = _mm_set_ps(1.0f, max.z, max.y, min.x); 
	mpBBVertexList[2] = _mm_set_ps(1.0f, min.z, max.y, min.x);
	mpBBVertexList[3] = _mm_set_ps(1.0f, min.z, max.y, max.x);
	// Bottom 4 vertices in BB
	mpBBVertexList[4] = _mm_set_ps(1.0f, min.z, min.y, max.x);
	mpBBVertexList[5] = _mm_set_ps(1.0f, max.z, min.y, max.x);
	mpBBVertexList[6] = _mm_set_ps(1.0f, max.z, min.y, min.x);
	mpBBVertexList[7] = _mm_set_ps(1.0f, min.z, min.y, min.x);
}

//----------------------------------------------------------------
// Determine is model is inside view frustum
//----------------------------------------------------------------
void TransformedAABBoxSSE::IsInsideViewFrustum(CPUTCamera *pCamera)
{
	mInsideViewFrustum = pCamera->mFrustum.IsVisible(mpCPUTModel->mBoundingBoxCenterWorldSpace, mpCPUTModel->mBoundingBoxHalfWorldSpace);
}

//----------------------------------------------------------------------------
// Determine if the occluddee size is too small and if so avoid drawing it
//----------------------------------------------------------------------------
bool TransformedAABBoxSSE::IsTooSmall(__m128 *pViewProjViewportMatrix, CPUTCamera *pCamera)
{
	float radius = mBBHalf.lengthSq(); // Use length-squared to avoid sqrt().  Relative comparissons hold.
	float fov = pCamera->GetFov();
	float tanOfHalfFov = tanf(fov * 0.5f);
	mTooSmall = false;

	MatrixMultiply(mWorldMatrix, pViewProjViewportMatrix, mCumulativeMatrix);

	__m128 center = _mm_set_ps(1.0f, mBBCenter.z, mBBCenter.y, mBBCenter.x);
	__m128 mBBCenterOSxForm = TransformCoords(&center, mCumulativeMatrix);
    float w = mBBCenterOSxForm.m128_f32[3];
	if( w > 1.0f )
	{
		float radiusDivW = radius / w;
		float r2DivW2DivTanFov = radiusDivW / tanOfHalfFov;

		mTooSmall = r2DivW2DivTanFov < (mOccludeeSizeThreshold * mOccludeeSizeThreshold) ?  true : false;
	}
	else
	{
		mTooSmall = false;
	}
	return mTooSmall;
}

//----------------------------------------------------------------
// Trasforms the AABB vertices to screen space once every frame
//----------------------------------------------------------------
void TransformedAABBoxSSE::TransformAABBox(__m128 *pXformedPos)
{
	for(UINT i = 0; i < AABB_VERTICES; i++)
	{
		pXformedPos[i] = TransformCoords(&mpBBVertexList[i], mCumulativeMatrix);
		float oneOverW = 1.0f/max(pXformedPos[i].m128_f32[3], 0.0000001f);
		pXformedPos[i] = pXformedPos[i] * oneOverW;
		pXformedPos[i].m128_f32[3] = oneOverW;
	}
}

void TransformedAABBoxSSE::Gather(vFloat4 pOut[3], UINT triId, const __m128 *pXformedPos)
{
	for(int lane = 0; lane < SSE; lane++)
	{
		for(int i = 0; i < 3; i++)
		{
			UINT index = sBBIndexList[(triId * 3) + (lane * 3) + i];
			pOut[i].X.m128_f32[lane] = pXformedPos[index].m128_f32[0];
			pOut[i].Y.m128_f32[lane] = pXformedPos[index].m128_f32[1];
			pOut[i].Z.m128_f32[lane] = pXformedPos[index].m128_f32[2];
			pOut[i].W.m128_f32[lane] = pXformedPos[index].m128_f32[3];
		}
	}
}

//-----------------------------------------------------------------------------------------
// Rasterize the occludee AABB and depth test it against the CPU rasterized depth buffer
// If any of the rasterized AABB pixels passes the depth test exit early and mark the occludee
// as visible. If all rasterized AABB pixels are occluded then the occludee is culled
//-----------------------------------------------------------------------------------------
void TransformedAABBoxSSE::RasterizeAndDepthTestAABBox(UINT *pRenderTargetPixels, const __m128 *pXformedPos)
{
	// Set DAZ and FZ MXCSR bits to flush denormals to zero (i.e., make it faster)
	// Denormal are zero (DAZ) is bit 6 and Flush to zero (FZ) is bit 15. 
	// so to enable the two to have to set bits 6 and 15 which 1000 0000 0100 0000 = 0x8040
	_mm_setcsr( _mm_getcsr() | 0x8040 );

	__m128i colOffset = _mm_setr_epi32(0, 1, 0, 1);
	__m128i rowOffset = _mm_setr_epi32(0, 0, 1, 1);

	float* pDepthBuffer = (float*)pRenderTargetPixels; 
	
	// Rasterize the AABB triangles 4 at a time
	for(UINT i = 0; i < AABB_TRIANGLES; i += SSE)
	{
		vFloat4 xformedPos[3];
		Gather(xformedPos, i, pXformedPos);

		// use fixed-point only for X and Y.  Avoid work for Z and W.
		__m128i fixX[3], fixY[3];
		for(int i = 0; i < 3; i++)
		{
			fixX[i] = _mm_cvtps_epi32(xformedPos[i].X);
			fixY[i] = _mm_cvtps_epi32(xformedPos[i].Y);
		}

		// Fab(x, y) =     Ax       +       By     +      C              = 0
		// Fab(x, y) = (ya - yb)x   +   (xb - xa)y + (xa * yb - xb * ya) = 0
		// Compute A = (ya - yb) for 2 of the 3 line segments that make up each triangle
		__m128i A1 = _mm_sub_epi32(fixY[2], fixY[0]);
		__m128i A2 = _mm_sub_epi32(fixY[0], fixY[1]);

		// Compute B = (xb - xa) for 2 of the 3 line segments that make up each triangle
		__m128i B1 = _mm_sub_epi32(fixX[0], fixX[2]);
		__m128i B2 = _mm_sub_epi32(fixX[1], fixX[0]);

		// Compute C = (xa * yb - xb * ya) for 2 of the 3 line segments that make up each triangle
		__m128i C1 = _mm_sub_epi32(_mm_mullo_epi32(fixX[2], fixY[0]), _mm_mullo_epi32(fixX[0], fixY[2]));
		__m128i C2 = _mm_sub_epi32(_mm_mullo_epi32(fixX[0], fixY[1]), _mm_mullo_epi32(fixX[1], fixY[0]));

		// Compute triangle area
		__m128i triArea = _mm_sub_epi32(_mm_mullo_epi32(B2, A1), _mm_mullo_epi32(B1, A2));
		__m128 oneOverTriArea = _mm_div_ps(_mm_set1_ps(1.0f), _mm_cvtepi32_ps(triArea));

		// Z setup
		__m128 Z[3];
		Z[0] = xformedPos[0].Z;
		Z[1] = _mm_mul_ps(_mm_sub_ps(xformedPos[1].Z, xformedPos[0].Z), oneOverTriArea);
		Z[2] = _mm_mul_ps(_mm_sub_ps(xformedPos[2].Z, xformedPos[0].Z), oneOverTriArea);

		// When we interpolate, beta and gama have already been advanced
		// by one block, so compensate here.
		Z[0] = _mm_sub_ps(Z[0], _mm_mul_ps(_mm_cvtepi32_ps(_mm_slli_epi32(A1, 1)), Z[1]));
		Z[0] = _mm_sub_ps(Z[0], _mm_mul_ps(_mm_cvtepi32_ps(_mm_slli_epi32(A2, 1)), Z[2]));

		// Use bounding box traversal strategy to determine which pixels to rasterize 
		__m128i startX = _mm_and_si128(Max(Min(Min(fixX[0], fixX[1]), fixX[2]), _mm_set1_epi32(0)), _mm_set1_epi32(0xFFFFFFFE));
		__m128i endX   = Min(Max(Max(fixX[0], fixX[1]), fixX[2]), _mm_set1_epi32(SCREENW-1));

		__m128i startY = _mm_and_si128(Max(Min(Min(fixY[0], fixY[1]), fixY[2]), _mm_set1_epi32(0)), _mm_set1_epi32(0xFFFFFFFE));
		__m128i endY   = Min(Max(Max(fixY[0], fixY[1]), fixY[2]), _mm_set1_epi32(SCREENH-1));

		for(int vv = 0; vv < 3; vv++) 
		{
            // If W (holding 1/w in our case) is not between 0 and 1,
            // then vertex is behind near clip plane (1.0 in our case.
            // If W < 1, then verify 1/W > 1 (for W>0), and 1/W < 0 (for W < 0).
		    __m128 nearClipMask0 = _mm_cmple_ps(xformedPos[vv].W, _mm_set1_ps(0.0f));
		    __m128 nearClipMask1 = _mm_cmpge_ps(xformedPos[vv].W, _mm_set1_ps(1.0f));
            __m128 nearClipMask  = _mm_or_ps(nearClipMask0, nearClipMask1);

			if(!_mm_test_all_zeros(*(__m128i*)&nearClipMask, *(__m128i*)&nearClipMask))
			{
                // All four vertices are behind the near plane (we're processing four triangles at a time w/ SSE)
                *mVisible = true;
                return;
			}
		}

		// Now we have 4 triangles set up.  Rasterize them each individually.
        for(int lane=0; lane < SSE; lane++)
        {
			// Skip triangle if area is zero 
			if(triArea.m128i_i32[lane] <= 0)
			{
				continue;
			}

			// Extract this triangle's properties from the SIMD versions
            __m128 zz[3];
			for(int vv = 0; vv < 3; vv++)
			{
				zz[vv] = _mm_set1_ps(Z[vv].m128_f32[lane]);
			}

			int startXx = startX.m128i_i32[lane];
			int endXx	= endX.m128i_i32[lane];
			int startYy = startY.m128i_i32[lane];
			int endYy	= endY.m128i_i32[lane];
		
			__m128i sum = _mm_set1_epi32(triArea.m128i_i32[lane]);

			__m128i aa1 = _mm_set1_epi32(A1.m128i_i32[lane]);
			__m128i aa2 = _mm_set1_epi32(A2.m128i_i32[lane]);

			__m128i bb1 = _mm_set1_epi32(B1.m128i_i32[lane]);
			__m128i bb2 = _mm_set1_epi32(B2.m128i_i32[lane]);

			__m128i cc1 = _mm_set1_epi32(C1.m128i_i32[lane]);
			__m128i cc2 = _mm_set1_epi32(C2.m128i_i32[lane]);

			__m128i aa1Inc = _mm_slli_epi32(aa1, 1);
			__m128i aa2Inc = _mm_slli_epi32(aa2, 1);
			__m128i aa0Dec = _mm_add_epi32(aa1Inc, aa2Inc);

			__m128i row, col;

			// Traverse pixels in 2x2 blocks and store 2x2 pixel quad depths contiguously in memory ==> 2*X
			// This method provides better perfromance
			int rowIdx = (startYy * SCREENW + 2 * startXx);
			int rowSamples = (endXx - startXx + 1) * 2;

			col = _mm_add_epi32(colOffset, _mm_set1_epi32(startXx));
			__m128i aa1Col = _mm_mullo_epi32(aa1, col);
			__m128i aa2Col = _mm_mullo_epi32(aa2, col);

			row = _mm_add_epi32(rowOffset, _mm_set1_epi32(startYy));
			__m128i bb1Row = _mm_add_epi32(_mm_mullo_epi32(bb1, row), cc1);
			__m128i bb2Row = _mm_add_epi32(_mm_mullo_epi32(bb2, row), cc2);

			__m128i bb1Inc = _mm_slli_epi32(bb1, 1);
			__m128i bb2Inc = _mm_slli_epi32(bb2, 1);

			// Incrementally compute Fab(x, y) for all the pixels inside the bounding box formed by (startX, endX) and (startY, endY)
			for(int r = startYy; r <= endYy; r += 2,
									 		row  = _mm_add_epi32(row, _mm_set1_epi32(2)),
											rowIdx = rowIdx + 2 * SCREENW,
											bb1Row = _mm_add_epi32(bb1Row, bb1Inc),
											bb2Row = _mm_add_epi32(bb2Row, bb2Inc))
			{
				// Compute barycentric coordinates 
				float *pDepthStart = &pDepthBuffer[rowIdx];
				float *pDepthEnd = pDepthStart + rowSamples;
				__m128i beta = _mm_add_epi32(aa1Col, bb1Row);
				__m128i gama = _mm_add_epi32(aa2Col, bb2Row);
				__m128i alpha = _mm_sub_epi32(_mm_sub_epi32(sum, beta), gama);

				for(float *pDepth = pDepthStart; pDepth < pDepthEnd; pDepth += 4)
				{
					//Test Pixel inside triangle
					__m128i mask = _mm_or_si128(_mm_or_si128(alpha, beta), gama);
					alpha = _mm_sub_epi32(alpha, aa0Dec);
					beta  = _mm_add_epi32(beta, aa1Inc);
					gama  = _mm_add_epi32(gama, aa2Inc);

					// Early out if all of this quad's pixels are outside the triangle.
					if(_mm_testc_si128(mask, _mm_set1_epi32(0x80000000)))
					{
						continue;
					}

					// Compute barycentric-interpolated depth
			        __m128 depth = zz[0];
					depth = _mm_add_ps(depth, _mm_mul_ps(_mm_cvtepi32_ps(beta), zz[1]));
					depth = _mm_add_ps(depth, _mm_mul_ps(_mm_cvtepi32_ps(gama), zz[2]));

					__m128 previousDepthValue = *(__m128*)pDepth;

					__m128 depthMask  = _mm_cmpge_ps( depth, previousDepthValue);
					__m128i finalMask = _mm_andnot_si128( mask, _mm_castps_si128(depthMask));
					if(!_mm_test_all_zeros(finalMask, finalMask))
					{
						*mVisible = true;
						return; //early exit
					}
				}//for each column											
			}// for each row
		}// for each triangle
	}// for each set of SIMD# triangles
}