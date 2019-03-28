#include "Framework.h"

#include "ShaderSet.h"
#include "Mesh.h"
#include "Texture.h"
#include <vector>

constexpr float kBlendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
constexpr UINT kSampleMask = 0xffffffff;
constexpr u32 kLightGridSize = 24;

//================================================================================
// Deferred Application
// An example of how to perform simple deferred rendering
//================================================================================
class DeferredApp : public FrameworkApp
{
public:

	struct PerFrameCBData
	{
		m4x4 m_matProjection;
		m4x4 m_matView;
		m4x4 m_matViewProjection;
		m4x4 m_matInverseProjection;
		m4x4 m_matInverseView;
		f32		m_time;
		f32     m_padding[3];
	};

	struct PerDrawCBData
	{
		m4x4 m_matMVP;
	};

	enum ELightType
	{
		kLightType_Directional,
		kLightType_Point,
		kLightType_Spot
	};

	// Light info presented to the shader constant buffer.
	struct LightInfo
	{
		v4 m_vPosition; // w == 0 then directional
		v4 m_vDirection; // for directional and spot , w == 0 then point.
		v4 m_vColour; // all types
		v4 m_vAtt; // attenuation factors + spot exponent in w
		// various spot params... to be added.
	};

	// A more general light management structure.
	struct Light
	{
		LightInfo m_shaderInfo;
		ELightType m_type;
	};

	void on_init(SystemsInterface& systems) override
	{
		m_position = v3(0.5f, 0.5f, 0.5f);
		m_size = 1.0f;
		systems.pCamera->eye = v3(10.f, 5.f, 7.f);
		systems.pCamera->look_at(v3(3.f, 0.5f, 0.f));

		create_shaders(systems);

		create_gbuffer(systems.pD3DDevice, systems.pD3DContext, systems.width, systems.height);

		// create fullscreen quad for post-fx / lighting passes. (-1, 1) in XY
		create_mesh_quad_xy(systems.pD3DDevice, m_fullScreenQuad, 1.0f);

		// Create Per Frame Constant Buffer.
		m_pPerFrameCB = create_constant_buffer<PerFrameCBData>(systems.pD3DDevice);

		// Create Per Draw Constant Buffer.
		m_pPerDrawCB = create_constant_buffer<PerDrawCBData>(systems.pD3DDevice);

		// Create Per Light Constant Buffer.
		m_pLightInfoCB = create_constant_buffer<LightInfo>(systems.pD3DDevice);

		// Initialize a mesh directly.
		create_mesh_cube(systems.pD3DDevice, m_meshArray[0], 0.5f);

		// Initialize a mesh from an .OBJ file
		create_mesh_from_obj(systems.pD3DDevice, m_meshArray[1], "Assets/Models/apple.obj", 0.01f);
		create_mesh_from_obj(systems.pD3DDevice, m_plane, "Assets/Models/plane.obj", 4.f);

		create_mesh_from_obj(systems.pD3DDevice, m_lightVolumeSphere, "Assets/Models/unit_sphere.obj", 1.f);

		// Initialise some textures;
		m_textureArray[0].init_from_dds(systems.pD3DDevice, "Assets/Textures/brick.dds");
		m_textureArray[1].init_from_dds(systems.pD3DDevice, "Assets/Textures/apple_diffuse.dds");

		// We need a sampler state to define wrapping and mipmap parameters.
		m_pSamplerState = create_basic_sampler(systems.pD3DDevice, D3D11_TEXTURE_ADDRESS_WRAP);

		// Setup per-frame data
		m_perFrameCBData.m_time = 0.0f;


		// create additive render states.
		{
			// Additive
			D3D11_BLEND_DESC desc = {};
			desc.AlphaToCoverageEnable = FALSE;
			desc.IndependentBlendEnable = FALSE;
			desc.RenderTarget[0].BlendEnable = TRUE;
			desc.RenderTarget[0].SrcBlend = desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
			desc.RenderTarget[0].DestBlend = desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
			desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

			systems.pD3DDevice->CreateBlendState(&desc, &m_pBlendStates[BlendStates::kAdditive]);

			// Opaque
			desc.RenderTarget[0].BlendEnable = FALSE;
			systems.pD3DDevice->CreateBlendState(&desc, &m_pBlendStates[BlendStates::kOpaque]);
		}

		create_lights();
	}

	void create_shaders(SystemsInterface &systems)
	{
		// Geometry pass shaders.
		m_geometryPassShader.init(systems.pD3DDevice
			, ShaderSetDesc::Create_VS_PS("Assets/Shaders/DeferredShaders.fx", "VS_Geometry", "PS_Geometry")
			, { VertexFormatTraits<MeshVertex>::desc, VertexFormatTraits<MeshVertex>::size }
		);

		// Lighting pass shaders
		m_directionalLightShader.init(systems.pD3DDevice
			, ShaderSetDesc::Create_VS_PS("Assets/Shaders/DeferredShaders.fx", "VS_Passthrough", "PS_DirectionalLight")
			, { VertexFormatTraits<MeshVertex>::desc, VertexFormatTraits<MeshVertex>::size }
		);

		// Lighting pass shaders
		m_pointLightShader.init(systems.pD3DDevice
			, ShaderSetDesc::Create_VS_PS("Assets/Shaders/DeferredShaders.fx", "VS_LightVolume", "PS_PointLight")
			, { VertexFormatTraits<MeshVertex>::desc, VertexFormatTraits<MeshVertex>::size }
		);

		// GBuffer Debugging shaders.
		m_GBufferDebugShaders[kGBufferDebug_Albido].init(systems.pD3DDevice
			, ShaderSetDesc::Create_VS_PS("Assets/Shaders/DeferredShaders.fx", "VS_Passthrough", "PS_GBufferDebug_Albido")
			, { VertexFormatTraits<MeshVertex>::desc, VertexFormatTraits<MeshVertex>::size }
		);
		m_GBufferDebugShaders[kGBufferDebug_Normals].init(systems.pD3DDevice
			, ShaderSetDesc::Create_VS_PS("Assets/Shaders/DeferredShaders.fx", "VS_Passthrough", "PS_GBufferDebug_Normals")
			, { VertexFormatTraits<MeshVertex>::desc, VertexFormatTraits<MeshVertex>::size }
		);
		m_GBufferDebugShaders[kGBufferDebug_Specular].init(systems.pD3DDevice
			, ShaderSetDesc::Create_VS_PS("Assets/Shaders/DeferredShaders.fx", "VS_Passthrough", "PS_GBufferDebug_Specular")
			, { VertexFormatTraits<MeshVertex>::desc, VertexFormatTraits<MeshVertex>::size }
		);
		m_GBufferDebugShaders[kGBufferDebug_Position].init(systems.pD3DDevice
			, ShaderSetDesc::Create_VS_PS("Assets/Shaders/DeferredShaders.fx", "VS_Passthrough", "PS_GBufferDebug_Position")
			, { VertexFormatTraits<MeshVertex>::desc, VertexFormatTraits<MeshVertex>::size }
		);
		m_GBufferDebugShaders[kGBufferDebug_Depth].init(systems.pD3DDevice
			, ShaderSetDesc::Create_VS_PS("Assets/Shaders/DeferredShaders.fx", "VS_Passthrough", "PS_GBufferDebug_Depth")
			, { VertexFormatTraits<MeshVertex>::desc, VertexFormatTraits<MeshVertex>::size }
		);
	}

	void create_lights()
	{	
		// A directional light.
		{
			Light l = {};
			l.m_shaderInfo.m_vDirection = v4(0.5773, 0.5773, 0.5773, 0);
			l.m_shaderInfo.m_vColour = v4(1.f, 0.7f, .6f, 0.f) * 0.2f;
			l.m_shaderInfo.m_vPosition = v4(0, 0, 0, 0);
			l.m_type = kLightType_Directional;

			m_lights.push_back(l);
		}

		// Lots of point lights.
		v4 colours[] =
		{
			v4(1,1,1,0),
			v4(1,1,0,0),
			v4(0,1,1,0),
			v4(1,0,1,0)
		};

		for (u32 i = 0; i < kLightGridSize; ++i)
		{
			for (u32 j = 0; j < kLightGridSize; ++j)
			{
				Light l = {};
				l.m_shaderInfo.m_vDirection = v4(0, 0, 0, 0);
				l.m_shaderInfo.m_vColour = colours[j % 5] * 0.9f;
				l.m_shaderInfo.m_vPosition = v4(i - 5.0, 0.5f, j - 5.0, 1.0f);
				l.m_shaderInfo.m_vAtt = v4(0.001f, 0.1f, 5.0f, 2.0f);
				l.m_type = kLightType_Point;

				m_lights.push_back(l);
			}
		}
	}

	void on_update(SystemsInterface& systems) override
	{
		//////////////////////////////////////////////////////////////////////////
		// You can use features from the ImGui library.
		// Investigate the ImGui::ShowDemoWindow() function for ideas.
		// see also : https://github.com/ocornut/imgui
		//////////////////////////////////////////////////////////////////////////

		// This function displays some useful debugging values, camera positions etc.
		DemoFeatures::editorHud(systems.pDebugDrawContext);

		ImGui::SliderFloat3("Position", (float*)&m_position, -1.f, 1.f);
		ImGui::SliderFloat("Size", &m_size, 0.1f, 10.f);

		// Update Per Frame Data.
		// calculate view project and inverse so we can project back from depth buffer into world coordinates.
		m4x4 matViewProj = systems.pCamera->viewMatrix * systems.pCamera->projMatrix;
		m4x4 matInverseProj = systems.pCamera->projMatrix.Invert();
		m4x4 matInverseView = systems.pCamera->viewMatrix.Invert();

		m_perFrameCBData.m_matProjection = systems.pCamera->projMatrix.Transpose();
		m_perFrameCBData.m_matView = systems.pCamera->viewMatrix.Transpose();
		m_perFrameCBData.m_matViewProjection = matViewProj.Transpose();
		m_perFrameCBData.m_matInverseProjection = matInverseProj.Transpose();
		m_perFrameCBData.m_matInverseView = matInverseView.Transpose();

		m_perFrameCBData.m_time += 0.001f;


		// move our lights
		for (u32 i = 0; i < kLightGridSize; ++i)
		{
			for (u32 j = 0; j < kLightGridSize; ++j)
			{
				m_lights[i* kLightGridSize + j + 1].m_shaderInfo.m_vPosition = v4(
					i + sin(i * m_perFrameCBData.m_time) - 5.0
					, cos(i * j * m_perFrameCBData.m_time) + 1
					, j + cos(j * m_perFrameCBData.m_time) - 5.0
					, 1.0f
				);
			}
		}
	}

	void on_render(SystemsInterface& systems) override
	{
		//////////////////////////////////////////////////////////////////////////
		// Imgui can also be used inside the render function.
		//////////////////////////////////////////////////////////////////////////


		//////////////////////////////////////////////////////////////////////////
		// You can use features from the DebugDrawlibrary.
		// Investigate the following functions for ideas.
		// see also : https://github.com/glampert/debug-draw
		//////////////////////////////////////////////////////////////////////////

		// Grid from -50 to +50 in both X & Z
		auto ctx = systems.pDebugDrawContext;

		//dd::xzSquareGrid(ctx, -50.0f, 50.0f, 0.0f, 1.f, dd::colors::DimGray);
		dd::axisTriad(ctx, (const float*)& m4x4::Identity, 0.1f, 15.0f);
		dd::box(ctx, (const float*)&m_position, dd::colors::Blue, m_size, m_size, m_size);
		if (systems.pCamera->pointInFrustum(m_position))
		{
			dd::projectedText(ctx, "A Box", (const float*)&m_position, dd::colors::White, (const float*)&systems.pCamera->vpMatrix, 0, 0, systems.width, systems.height, 0.5f);
		}

		// Push Per Frame Data to GPU
		D3D11_MAPPED_SUBRESOURCE subresource;
		if (!FAILED(systems.pD3DContext->Map(m_pPerFrameCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &subresource)))
		{
			memcpy(subresource.pData, &m_perFrameCBData, sizeof(PerFrameCBData));
			systems.pD3DContext->Unmap(m_pPerFrameCB, 0);
		}

		for (auto& rLight : m_lights)
		{
			dd::cross(ctx, (const float*)& rLight.m_shaderInfo.m_vPosition, 0.2f);
		}


		//=======================================================================================
		// The Geometry Pass.
		// Draw our scene into the GBuffer, capturing all the information we need for lighting.
		//=======================================================================================

		// Bind the G Buffer to the output merger
		// Here we are binding multiple render targets (MRT)
		systems.pD3DContext->OMSetRenderTargets(kMaxGBufferColourTargets, m_pGBufferTargetViews, m_pGBufferDepthView);

		// Clear colour and depth
		f32 clearValue[] = { 0.f, 0.f, 0.f, 0.f };
		systems.pD3DContext->ClearRenderTargetView(m_pGBufferTargetViews[kGBufferColourSpec], clearValue);
		f32 normalClearValue[] = { 0.5f, 0.5f, 0.5f, 0.f };
		systems.pD3DContext->ClearRenderTargetView(m_pGBufferTargetViews[kGBufferNormalPow], normalClearValue);
		systems.pD3DContext->ClearDepthStencilView(m_pGBufferDepthView, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.f, 0);

		// Bind our geometry pass shader.
		m_geometryPassShader.bind(systems.pD3DContext);

		// Bind Constant Buffers, to both PS and VS stages
		ID3D11Buffer* buffers[] = { m_pPerFrameCB, m_pPerDrawCB };
		systems.pD3DContext->VSSetConstantBuffers(0, 2, buffers);
		systems.pD3DContext->PSSetConstantBuffers(0, 2, buffers);

		// Bind a sampler state
		ID3D11SamplerState* samplers[] = { m_pSamplerState };
		systems.pD3DContext->PSSetSamplers(0, 1, samplers);


		// Opaque blend
		systems.pD3DContext->OMSetBlendState(m_pBlendStates[BlendStates::kOpaque], kBlendFactor, kSampleMask);

		// draw a plane
		{			
			m_plane.bind(systems.pD3DContext);
			m_textureArray[0].bind(systems.pD3DContext, ShaderStage::kPixel, 0);

			// Compute MVP matrix.
			m4x4 matModel = m4x4::CreateTranslation(0.f, 0.f, 0.f);
			m4x4 matMVP = matModel * systems.pCamera->vpMatrix;

			// Update Per Draw Data
			m_perDrawCBData.m_matMVP = matMVP.Transpose();

			// Push to GPU
			push_constant_buffer(systems.pD3DContext, m_pPerDrawCB, m_perDrawCBData);

			// Draw the mesh.
			m_plane.draw(systems.pD3DContext);

		}


		constexpr f32 kGridSpacing = 1.5f;
		constexpr u32 kNumInstances = 5;
		constexpr u32 kNumModelTypes = 2;

		for (u32 i = 0; i < kNumModelTypes; ++i)
		{
			// Bind a mesh and texture.
			m_meshArray[i].bind(systems.pD3DContext);
			m_textureArray[i].bind(systems.pD3DContext, ShaderStage::kPixel, 0);

			// Draw several instances
			for (u32 j = 0; j < kNumInstances; ++j)
			{
				// Compute MVP matrix.
				m4x4 matModel = m4x4::CreateTranslation(v3(j * kGridSpacing, i * kGridSpacing, 0.f));
				m4x4 matMVP = matModel * systems.pCamera->vpMatrix;

				// Update Per Draw Data
				m_perDrawCBData.m_matMVP = matMVP.Transpose();

				// Push to GPU
				push_constant_buffer(systems.pD3DContext, m_pPerDrawCB, m_perDrawCBData);

				// Draw the mesh.
				m_meshArray[i].draw(systems.pD3DContext);
			}
		}

		//=======================================================================================
		// The Lighting
		// Read the GBuffer textures, and "draw" light volumes for each of our lights.
		// We use additive blending on the result.
		//=======================================================================================

		// Bind the swap chain (back buffer) to the render target
		// Make sure to unbind other gbuffer targets and depth
		ID3D11RenderTargetView* views[] = { systems.pSwapRenderTarget, 0 };
		systems.pD3DContext->OMSetRenderTargets(2, views, 0);

		// Bind our GBuffer textures as inputs to the pixel shader
		systems.pD3DContext->PSSetShaderResources(0, 3, m_pGBufferTextureViews);


		// For exploring the GBuffer data we use a shader.
		// Bind GBuffer Debugging shader.
		static int sel = 0;
		static bool bDebugEnabled = true;
		ImGui::Checkbox("GBuffer Debug Enable", &bDebugEnabled);
		if (bDebugEnabled)
		{
			const char* aModeNames[] = { "Albido","Normals","Specular","Position","Depth" };
			ImGui::ListBox("GBuffer Debug Mode", &sel, aModeNames, kMaxGBufferDebugModes);

			m_GBufferDebugShaders[sel].bind(systems.pD3DContext);

			// ... and draw a full screen quad.
			m_fullScreenQuad.bind(systems.pD3DContext);
			m_fullScreenQuad.draw(systems.pD3DContext);
		}
		else
		{
			// if we are not debugging the we bind the lighting shader and start accumulating light volumes.
			// bind the light constant buffer
			systems.pD3DContext->PSSetConstantBuffers(2, 1, &m_pLightInfoCB);

			// Additive blend so we accumulate
			systems.pD3DContext->OMSetBlendState(m_pBlendStates[BlendStates::kAdditive], kBlendFactor, kSampleMask);

			static v4 tuneAtt(0.001f, 0.1f, 15.0f, 0.5f);
			ImGui::DragFloat4("Light Att", (float*)&tuneAtt, 0.0001, 5.0f);


			static int maxLights = m_lights.size();
			ImGui::SliderInt("Lights", &maxLights, 0, m_lights.size());

			for (u32 i = 0; i < (u32)maxLights; ++i)
			{
				auto& rLight(m_lights[i]);
				// For drawing a directional light which hits everywhere we draw a full screen quad.

				// Update and the light info constants.
			//	rLight.m_shaderInfo.m_vAtt = tuneAtt;
				push_constant_buffer(systems.pD3DContext, m_pLightInfoCB, rLight.m_shaderInfo);

				switch (rLight.m_type)
				{
				case kLightType_Directional:
				{
					m_directionalLightShader.bind(systems.pD3DContext);
					m_fullScreenQuad.bind(systems.pD3DContext);
					m_fullScreenQuad.draw(systems.pD3DContext);
				}
				break;
				case kLightType_Point:
				{
					m_pointLightShader.bind(systems.pD3DContext);

					// Compute Light MVP matrix.
					m4x4 matModel = m4x4::CreateScale(rLight.m_shaderInfo.m_vAtt.w); 
					matModel *= m4x4::CreateTranslation(v3(rLight.m_shaderInfo.m_vPosition));
					m4x4 matMVP = matModel * systems.pCamera->vpMatrix;

					// Update Per Draw Data
					m_perDrawCBData.m_matMVP = matMVP.Transpose();
					push_constant_buffer(systems.pD3DContext, m_pPerDrawCB, m_perDrawCBData);

					m_lightVolumeSphere.bind(systems.pD3DContext);
					m_lightVolumeSphere.draw(systems.pD3DContext);
				}
				break;
				case kLightType_Spot:
					break;
				default:
					break;

				}


			}
		}




		// Unbind all the SRVs because we need them as targets next frame
		ID3D11ShaderResourceView* srvClear[] = { 0,0,0 };
		systems.pD3DContext->PSSetShaderResources(0, 3, srvClear);

		// re-bind depth for debugging output.
		systems.pD3DContext->OMSetRenderTargets(2, views, m_pGBufferDepthView);
	}

	void on_resize(SystemsInterface& systems) override
	{
		create_gbuffer(systems.pD3DDevice, systems.pD3DContext, systems.width, systems.height);
	}

private:

	enum EGBufferConstants
	{
		kGBufferColourSpec, // f16 Target, Albido Colour RGB + Specular Intensity.
		kGBufferNormalPow, // f16 Target Nsormal + Specular Power.
		kGBufferDepth, // f32 Depth Target.

		kMaxGBufferColourTargets = 2,
		kMaxGBufferTextures = 3
	};

	enum EGBufferDebugModes
	{
		kGBufferDebug_Albido,
		kGBufferDebug_Normals,
		kGBufferDebug_Specular,
		kGBufferDebug_Position,
		kGBufferDebug_Depth,
		kMaxGBufferDebugModes
	};

	void create_gbuffer(ID3D11Device* pD3DDevice, ID3D11DeviceContext* pD3DContext, u32 width, u32 height)
	{
		HRESULT hr;

		// Release all outstanding references to the swap chain's buffers.
		pD3DContext->OMSetRenderTargets(0, 0, 0);

		// destroy old g-buffer views.
		SAFE_RELEASE(m_pGBufferDepthView);

		for (u32 i = 0; i < kMaxGBufferColourTargets; ++i)
		{
			SAFE_RELEASE(m_pGBufferTargetViews[i]);
		}

		// destroy old g-buffer textures.
		for (u32 i = 0; i < kMaxGBufferTextures; ++i)
		{
			SAFE_RELEASE(m_pGBufferTexture[i]);
			SAFE_RELEASE(m_pGBufferTextureViews[i]);
		}

		// Create a colour buffers
		for (u32 i = 0; i < kMaxGBufferColourTargets; ++i)
		{
			D3D11_TEXTURE2D_DESC desc;
			desc.Width = width;
			desc.Height = height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; // 4 component f16 targets
			desc.SampleDesc.Count = 1;
			desc.SampleDesc.Quality = 0;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			desc.CPUAccessFlags = 0;
			desc.MiscFlags = 0;

			hr = pD3DDevice->CreateTexture2D(&desc, NULL, &m_pGBufferTexture[i]);
			if (FAILED(hr))
			{
				panicF("Failed colour texture for GBuffer");
			}

			// render target views.
			hr = pD3DDevice->CreateRenderTargetView(m_pGBufferTexture[i], NULL, &m_pGBufferTargetViews[i]);
			if (FAILED(hr))
			{
				panicF("Failed colour target view for GBuffer");
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = desc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MostDetailedMip = 0;
			srvDesc.Texture2D.MipLevels = 1;

			hr = pD3DDevice->CreateShaderResourceView(m_pGBufferTexture[i], &srvDesc, &m_pGBufferTextureViews[i]);
			if (FAILED(hr))
			{
				panicF("Failed to create SRV of Target for GBuffer");
			}

		}

		// Create a depth buffer
		{
			D3D11_TEXTURE2D_DESC desc;
			desc.Width = width;
			desc.Height = height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R24G8_TYPELESS; // Typeless because we are binding as SRV and DepthStencilView
			desc.SampleDesc.Count = 1;
			desc.SampleDesc.Quality = 0;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
			desc.CPUAccessFlags = 0;
			desc.MiscFlags = 0;

			hr = pD3DDevice->CreateTexture2D(&desc, NULL, &m_pGBufferTexture[kGBufferDepth]);
			if (FAILED(hr))
			{
				panicF("Failed to create Depth Buffer for GBuffer");
			}

			D3D11_DEPTH_STENCIL_VIEW_DESC depthDesc = {};
			depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; // View suitable for writing depth
			depthDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
			depthDesc.Texture2D.MipSlice = 0;

			hr = pD3DDevice->CreateDepthStencilView(m_pGBufferTexture[kGBufferDepth], &depthDesc, &m_pGBufferDepthView);
			if (FAILED(hr))
			{
				panicF("Failed to create Depth Stencil View for GBuffer");
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; // View suitable for decoding full 24bits of depth to red channel.
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MostDetailedMip = 0;
			srvDesc.Texture2D.MipLevels = 1;

			hr = pD3DDevice->CreateShaderResourceView(m_pGBufferTexture[kGBufferDepth], &srvDesc, &m_pGBufferTextureViews[kGBufferDepth]);
			if (FAILED(hr))
			{
				panicF("Failed to create SRV of Depth for GBuffer");
			}
		}
	}

private:

	enum BlendStates
	{
		kOpaque,
		kAdditive,
		kMaxBlendStates
	};
	ID3D11BlendState* m_pBlendStates[kMaxBlendStates];

	PerFrameCBData m_perFrameCBData;
	ID3D11Buffer* m_pPerFrameCB = nullptr;

	PerDrawCBData m_perDrawCBData;
	ID3D11Buffer* m_pPerDrawCB = nullptr;


	std::vector<Light> m_lights;
	ID3D11Buffer* m_pLightInfoCB = nullptr;


	ShaderSet m_geometryPassShader;
	ShaderSet m_directionalLightShader;
	ShaderSet m_pointLightShader;
	ShaderSet m_GBufferDebugShaders[kMaxGBufferDebugModes];

	// Scene related objects
	Mesh m_meshArray[2];
	Texture m_textureArray[2];
	ID3D11SamplerState* m_pSamplerState = nullptr;

	Mesh m_plane;

	// Screen quad : for deferred passes
	Mesh m_fullScreenQuad;
	Mesh m_lightVolumeSphere;


	// GBuffer objects
	ID3D11Texture2D*		m_pGBufferTexture[kMaxGBufferTextures];
	ID3D11RenderTargetView* m_pGBufferTargetViews[kMaxGBufferColourTargets];
	ID3D11DepthStencilView* m_pGBufferDepthView;
	ID3D11ShaderResourceView* m_pGBufferTextureViews[kMaxGBufferTextures];


	v3 m_position;
	f32 m_size;

};

DeferredApp g_app;

FRAMEWORK_IMPLEMENT_MAIN(g_app, "Deferred")
