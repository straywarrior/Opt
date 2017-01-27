#pragma once

#include "mLibInclude.h"

#include <cuda_runtime.h>
#include <cudaUtil.h>

#include "TerraSolver.h"
#include "CUDATimer.h"
#include "../../shared/CombinedSolverParameters.h"
#include "../../shared/SolverIteration.h"

class ImageWarping
{
	public:
	
		ImageWarping(const ColorImageR32G32B32A32& image)
		{
			m_image = image;

			cutilSafeCall(cudaMalloc(&d_input, sizeof(float3)*m_image.getWidth()*m_image.getHeight()));
			cutilSafeCall(cudaMalloc(&d_targetFloat3, sizeof(float3)*m_image.getWidth()*m_image.getHeight()));
			cutilSafeCall(cudaMalloc(&d_imageFloat3Albedo,  sizeof(float3)*m_image.getWidth()*m_image.getHeight()));
			cutilSafeCall(cudaMalloc(&d_imageFloatIllumination, sizeof(float)*m_image.getWidth()*m_image.getHeight()));
						
			resetGPUMemory();

            m_gnSolver = new TerraSolver(m_image.getWidth(), m_image.getHeight(), "SmoothingLaplacianFloat3AD.t", "gaussNewtonGPU");
            m_lmSolver = new TerraSolver(m_image.getWidth(), m_image.getHeight(), "SmoothingLaplacianFloat3AD.t", "LMGPU");
		}

		void resetGPUMemory()
		{
			float3* h_input = new float3[m_image.getWidth()*m_image.getHeight()];
			float3* h_imageFloat3 = new float3[m_image.getWidth()*m_image.getHeight()];
			float3* h_imageFloat3Albedo = new float3[m_image.getWidth()*m_image.getHeight()];
			float*  h_imageFloatIllumination = new float[m_image.getWidth()*m_image.getHeight()];
			
			for (unsigned int i = 0; i < m_image.getHeight(); i++)
			{
				for (unsigned int j = 0; j < m_image.getWidth(); j++)
				{
					float EPS = 0.01f;

					ml::vec4f v = m_image(j, i);
					v = v / 255.0f;

					float intensity = (v.x + v.y + v.z) / 3.0f;
					ml::vec4f chroma = v / intensity;

					ml::vec4f t = m_image(j, i);
					t = t / 255.0f;

					t.x = log2(t.x + EPS);
					t.y = log2(t.y + EPS);
					t.z = log2(t.z + EPS);
					t.w = 0.0f;
					
					intensity = log2(intensity + EPS);
					
					chroma.x = log2(chroma.x + EPS);
					chroma.y = log2(chroma.y + EPS);
					chroma.z = log2(chroma.z + EPS);
	
					h_input[i*m_image.getWidth() + j] = make_float3(v.x, v.y, v.z);
					h_imageFloat3[i*m_image.getWidth() + j] = make_float3(t.x, t.y, t.z);
					h_imageFloat3Albedo[i*m_image.getWidth() + j] = make_float3(chroma.x, chroma.y, chroma.z);
					h_imageFloatIllumination[i*m_image.getWidth() + j] = intensity;
				}
			}

			cutilSafeCall(cudaMemcpy(d_input, h_input, sizeof(float3)*m_image.getWidth()*m_image.getHeight(), cudaMemcpyHostToDevice));
			cutilSafeCall(cudaMemcpy(d_targetFloat3, h_imageFloat3, sizeof(float3)*m_image.getWidth()*m_image.getHeight(), cudaMemcpyHostToDevice));
			cutilSafeCall(cudaMemcpy(d_imageFloat3Albedo, h_imageFloat3Albedo, sizeof(float3)*m_image.getWidth()*m_image.getHeight(), cudaMemcpyHostToDevice));
			cutilSafeCall(cudaMemcpy(d_imageFloatIllumination, h_imageFloatIllumination, sizeof(float)*m_image.getWidth()*m_image.getHeight(), cudaMemcpyHostToDevice));

			delete h_input;
			delete h_imageFloat3;
			delete h_imageFloat3Albedo;
			delete h_imageFloatIllumination;
		}

		~ImageWarping()
		{
			cutilSafeCall(cudaFree(d_input));
			cutilSafeCall(cudaFree(d_targetFloat3));
			cutilSafeCall(cudaFree(d_imageFloat3Albedo));
			cutilSafeCall(cudaFree(d_imageFloatIllumination));

			SAFE_DELETE(m_gnSolver);
            SAFE_DELETE(m_lmSolver);
		}

		ColorImageR32G32B32A32* solve()
		{
			float weightFit  = 500.0f;
			float weightRegAlbedo  = 1000.0f;
			float weightRegShading = 10000.0f;
			float weightRegChroma  = 100.0f;
			float pNorm = 0.8;

            m_params.nonLinearIter = 7;
            m_params.linearIter = 10;
	         
            if (m_params.useOpt) {
                std::cout << "\n\nOPT GN" << std::endl;
                resetGPUMemory();
                m_gnSolver->solve(d_imageFloat3Albedo, d_imageFloatIllumination, d_targetFloat3, d_input, m_params.nonLinearIter, m_params.linearIter, 0, weightFit, weightRegAlbedo, weightRegShading, weightRegChroma, pNorm);
            }
            if (m_params.useOptLM) {
                std::cout << "\n\nOPT LM" << std::endl;
                resetGPUMemory();
                m_lmSolver->solve(d_imageFloat3Albedo, d_imageFloatIllumination, d_targetFloat3, d_input, m_params.nonLinearIter, m_params.linearIter, 0, weightFit, weightRegAlbedo, weightRegShading, weightRegChroma, pNorm);
            }

            reportFinalCosts("Intrinsic Images", m_params, m_gnSolver->finalCost(), m_lmSolver->finalCost(), nan(nullptr));
			copyResultToCPUFromFloat3();			
			return &m_result;
		}

		ColorImageR32G32B32A32* getAlbedo()
		{
			return &m_result;
		}

		ColorImageR32G32B32A32* getShading()
		{
			return &m_resultShading;
		}

		void copyResultToCPUFromFloat3()
		{
			m_result = ColorImageR32G32B32A32(m_image.getWidth(), m_image.getHeight());

			float3* h_imageFloat3 = new float3[m_image.getWidth()*m_image.getHeight()];
			cutilSafeCall(cudaMemcpy(h_imageFloat3, d_imageFloat3Albedo, sizeof(float3)*m_image.getWidth()*m_image.getHeight(), cudaMemcpyDeviceToHost));

			for (unsigned int i = 0; i < m_image.getHeight(); i++)
			{
				for (unsigned int j = 0; j < m_image.getWidth(); j++)
				{
					float3 v = h_imageFloat3[i*m_image.getWidth() + j];
					v.x = exp2(v.x) / 1.5f;
					v.y = exp2(v.y) / 1.5f;
					v.z = exp2(v.z) / 1.5f;
					m_result(j, i) = vec4f(v.x, v.y, v.z, 1.0f);
				}
			}

			delete h_imageFloat3;

			m_resultShading = ColorImageR32G32B32A32(m_image.getWidth(), m_image.getHeight());
			
			float* h_imageFloatShading = new float[m_image.getWidth()*m_image.getHeight()];
			cutilSafeCall(cudaMemcpy(h_imageFloatShading, d_imageFloatIllumination, sizeof(float)*m_image.getWidth()*m_image.getHeight(), cudaMemcpyDeviceToHost));
			
			for (unsigned int i = 0; i < m_image.getHeight(); i++)
			{
				for (unsigned int j = 0; j < m_image.getWidth(); j++)
				{
					float v = h_imageFloatShading[i*m_image.getWidth() + j];
					v = exp2(v);
					m_resultShading(j, i) = vec4f(v, v, v, 1.0f);
				}
			}
			
			delete h_imageFloatShading;
		}

	private:

		ColorImageR32G32B32A32 m_result;
		ColorImageR32G32B32A32 m_resultShading;
		ColorImageR32G32B32A32 m_image;
	
		float3*	d_imageFloat3Albedo;
		float*	d_imageFloatIllumination;
		float3* d_targetFloat3;
		float3* d_input;
	

        CombinedSolverParameters m_params;
        TerraSolver* m_gnSolver;
        TerraSolver* m_lmSolver;

};