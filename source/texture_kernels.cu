//--------------------------------------------------------------------------------
//
//	Redistribution and use in source and binary forms, with or without
//	modification, are permitted provided that the following conditions are met :
//
//	*Redistributions of source code must retain the above copyright notice, this
//	list of conditions and the following disclaimer.
//
//	* Redistributions in binary form must reproduce the above copyright notice,
//	this list of conditions and the following disclaimer in the documentation
//	and/or other materials provided with the distribution.
//	
//	* Neither the name of the copyright holder nor the names of its
//	contributors may be used to endorse or promote products derived from
//	this software without specific prior written permission.
//	
//	THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
//	AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
//	IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
//	DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
//	FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
//	DAMAGES(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
//	SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
//	CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
//	OR TORT(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
//	OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// 
// Copyright(c) 2019, Sergen Eren
// All rights reserved.
//----------------------------------------------------------------------------------
// 
//	Version 1.0: Sergen Eren, 02/11/2019
//
// File: Kernels to calculate and load the procedural sky value and cdf textures
//
//-----------------------------------------------

#define _USE_MATH_DEFINES
#include <cmath>
#include <stdio.h>
#include <float.h>

// Cuda includes
#include <cuda_runtime.h> 
#include <curand_kernel.h>
#include <device_launch_parameters.h>
#include "helper_math.h"


// Internal includes
#include "kernel_params.h"
#include "cuda_noise.cuh"

#define INV_2_PI		1.0f / (2.0f * M_PI) 
#define INV_4_PI		1.0f / (4.0f * M_PI) 
#define INV_PI			1.0f / M_PI 


typedef curandStatePhilox4_32_10_t Rand_state;
#define rand(state) curand_uniform(state)



extern "C" __global__ void glow(const Kernel_params kernel_params, float treshold , const int width, const int height) {

	int x = blockIdx.x * blockDim.x + threadIdx.x;
	int y = blockIdx.y * blockDim.y + threadIdx.y;
	if (x >= width || y >= height) return;
	const unsigned int idx = y * width + x;

	// TODO gaussian blur and add glow effect to display buffer 


}

extern "C" __global__ void fill_volume_buffer( float *buffer, const int3 dims, const float scale, const int noise_type) {

	int x = blockIdx.x * blockDim.x + threadIdx.x;
	int y = blockIdx.y * blockDim.y + threadIdx.y;
	int z = blockIdx.z * blockDim.z + threadIdx.z;

	if (x >= dims.x || y >= dims.y || z >= dims.z) return;

	const unsigned int idx = x + dims.x * (y + dims.y * z);

	Rand_state rand_state;

	int seed = 123;
	float du = 1.0f / (float)dims.x;

	float dx = cudaNoise::randomFloat(482 + floor(rand(&rand_state) * 2) * 47 + seed) / (float)dims.x;
	float dy = cudaNoise::randomFloat(472 + floor(rand(&rand_state) * 2) * 38 + seed) / (float)dims.y;
	float dz = cudaNoise::randomFloat(348 + floor(rand(&rand_state) * 2) * 14 + seed) / (float)dims.z;

	float3 pos = make_float3(x+dx, y+dy, z+dz);
	
	switch (noise_type)
	{
	case(0):
		buffer[idx] = cudaNoise::perlinNoise(pos, scale, seed);
		break;
	case(1):
		buffer[idx] = cudaNoise::simplexNoise(pos, scale, seed);
		break;
	case(2):
		buffer[idx] = cudaNoise::worleyNoise(pos, scale, seed, 300.1f, 4, 4, 1.0f);
		break;
	case(3):
		buffer[idx] = cudaNoise::repeaterPerlin(pos, scale, seed, 128, 1.9f, 0.5f);
		break;
	case(4):
		buffer[idx] = cudaNoise::repeaterPerlinAbs(pos, scale, seed, 128, 1.9f, 0.5f);
		break;
	case(5):
		buffer[idx] = cudaNoise::fractalSimplex(pos, scale, seed, du, 512, 1.5f, 0.95f);
		break;
	case(6):
		buffer[idx] = cudaNoise::repeaterTurbulence(pos, 0.2f, scale, seed, 0.8f, 32, cudaNoise::BASIS_PERLIN, cudaNoise::BASIS_PERLIN);
		break;
	case(7):
		buffer[idx] = cudaNoise::cubicValue(pos, scale, seed);
		break;
	case(8):
		buffer[idx] = cudaNoise::spots(pos, scale, seed, 0.1f, 0, 8, 1.0f, cudaNoise::SHAPE_STEP);
		break;
	}

}