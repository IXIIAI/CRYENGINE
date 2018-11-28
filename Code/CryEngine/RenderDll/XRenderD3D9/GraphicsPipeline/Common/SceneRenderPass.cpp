// Copyright 2001-2018 Crytek GmbH / Crytek Group. All rights reserved.

#include "StdAfx.h"
#include "SceneRenderPass.h"

#include "Common/RenderView.h"
#include "Common/ReverseDepth.h"
#include "CompiledRenderObject.h"
#include "GraphicsPipelineStage.h"

#include <algorithm>
#include <iterator>
#include <vector>

int CSceneRenderPass::s_recursionCounter = 0;

CSceneRenderPass::CSceneRenderPass()
	: m_renderPassDesc()
	, m_passFlags(ePassFlags_None)
{
	m_numRenderItemGroups = 0;

	SetLabel("SCENE_PASS");
}

void CSceneRenderPass::SetupDrawContext(uint32 stageID, uint32 stagePassID, EShaderTechniqueID technique, uint32 includeFilter, uint32 excludeFilter)
{
	m_stageID = stageID;
	m_passID = stagePassID;
	m_technique = technique;
	m_batchIncludeFilter = includeFilter;
	m_batchExcludeFilter = excludeFilter;
}

void CSceneRenderPass::SetPassResources(CDeviceResourceLayoutPtr pResourceLayout, CDeviceResourceSetPtr pPerPassResources)
{
	CRY_ASSERT_MESSAGE(!!pResourceLayout, "Layout to be set turns out to be invalid!");

	m_pResourceLayout = pResourceLayout;
	m_pPerPassResourceSet = pPerPassResources;
}

void CSceneRenderPass::SetRenderTargets(CTexture* pDepthTarget, CTexture* pColorTarget0, CTexture* pColorTarget1, CTexture* pColorTarget2, CTexture* pColorTarget3)
{
	m_renderPassDesc.SetRenderTarget(0, pColorTarget0);
	m_renderPassDesc.SetRenderTarget(1, pColorTarget1);
	m_renderPassDesc.SetRenderTarget(2, pColorTarget2);
	m_renderPassDesc.SetRenderTarget(3, pColorTarget3);
	m_renderPassDesc.SetDepthTarget(pDepthTarget);

	CRY_ASSERT_MESSAGE(
		(!pColorTarget0 || !pColorTarget1 || pColorTarget0->GetWidth() == pColorTarget1->GetWidth()) &&
		(!pColorTarget1 || !pColorTarget2 || pColorTarget1->GetWidth() == pColorTarget2->GetWidth()) &&
		(!pColorTarget2 || !pColorTarget3 || pColorTarget2->GetWidth() == pColorTarget3->GetWidth()),
		"Color targets are of different size!");
	CRY_ASSERT_MESSAGE(
		(!pDepthTarget  || !pColorTarget0 || pDepthTarget ->GetWidth() >= pColorTarget0->GetWidth()),
		"Depth target is smaller than the color target(s)!");

	// TODO: refactor, shouldn't need to update the renderpass here but PSOs are compiled before CSceneRenderPass::Prepare is called
	CDeviceRenderPass::UpdateWithReevaluation(m_pRenderPass, m_renderPassDesc);
}

void CSceneRenderPass::SetViewport(const D3DViewPort& viewport)
{
	m_viewPort[0] =
	m_viewPort[1] = viewport;

	if (m_passFlags & CSceneRenderPass::ePassFlags_RenderNearest)
	{
		m_viewPort[1].MinDepth = 0;
		m_viewPort[1].MaxDepth = CRenderer::CV_r_DrawNearZRange;
		if (m_passFlags & CSceneRenderPass::ePassFlags_ReverseDepth)
			m_viewPort[1] = ReverseDepthHelper::Convert(m_viewPort[1]);
	}

	D3DRectangle scissorRect =
	{
		LONG(m_viewPort[0].TopLeftX),
		LONG(m_viewPort[0].TopLeftY),
		LONG(m_viewPort[0].TopLeftX + m_viewPort[0].Width),
		LONG(m_viewPort[0].TopLeftY + m_viewPort[0].Height)
	};

	m_scissorRect = scissorRect;
}

void CSceneRenderPass::SetViewport(const SRenderViewport& viewport)
{
	SetViewport(RenderViewportToD3D11Viewport(viewport));
}

void CSceneRenderPass::SetDepthBias(float constBias, float slopeBias, float biasClamp)
{ 
	m_depthConstBias = constBias; 
	m_depthSlopeBias = slopeBias; 
	m_depthBiasClamp = biasClamp; 
}

void CSceneRenderPass::ExchangeRenderTarget(uint32 slot, CTexture* pNewColorTarget, ResourceViewHandle hRenderTargetView)
{
	CRY_ASSERT(!pNewColorTarget || pNewColorTarget->GetDevTexture());
	m_renderPassDesc.SetRenderTarget(slot, pNewColorTarget, hRenderTargetView);
}

void CSceneRenderPass::ExchangeDepthTarget(CTexture* pNewDepthTarget, ResourceViewHandle hDepthStencilView)
{
	CRY_ASSERT(!pNewDepthTarget || pNewDepthTarget->GetDevTexture());
	m_renderPassDesc.SetDepthTarget(pNewDepthTarget, hDepthStencilView);
}

void CSceneRenderPass::PrepareRenderPassForUse(CDeviceCommandListRef RESTRICT_REFERENCE commandList)
{
	CDeviceRenderPass::UpdateWithReevaluation(m_pRenderPass, m_renderPassDesc);

	CDeviceGraphicsCommandInterface* pCommandInterface = commandList.GetGraphicsInterface();
	pCommandInterface->PrepareRenderPassForUse(*m_pRenderPass);
	pCommandInterface->PrepareResourcesForUse(EResourceLayoutSlot_PerPassRS, m_pPerPassResourceSet.get());

	if (m_passFlags & ePassFlags_VrProjectionPass)
	{
		if (CVrProjectionManager::IsMultiResEnabledStatic())
		{
			// we don't know the bNearest flag here, so just prepare for both cases
			CVrProjectionManager::Instance()->PrepareProjectionParameters(commandList, GetViewport(false));
			CVrProjectionManager::Instance()->PrepareProjectionParameters(commandList, GetViewport(true));
		}
	}
}

void CSceneRenderPass::ResolvePass(CDeviceCommandListRef RESTRICT_REFERENCE commandList, const std::vector<TRect_tpl<uint16>>& screenBounds) const
{
	CDeviceGraphicsCommandInterface* pCommandInterface = commandList.GetGraphicsInterface();

	const auto textureWidth = CRendererResources::s_ptexHDRTarget->GetWidth();
	const auto textureHeight = CRendererResources::s_ptexHDRTarget->GetHeight();

	for (const auto &bounds : screenBounds)
	{
		const auto x = bounds.Min.x;
		const auto y = bounds.Min.y;
		const auto w = bounds.Max.x - x;
		const auto h = bounds.Max.y - y;

		SResourceCoordinate region = { x, y, 0, 0 };
		SResourceRegionMapping mapping =
		{
			region,   // src position
			region,   // dst position
			{
				static_cast<UINT>(std::max<int>(0, std::min<int>(w, textureWidth - x))),
				static_cast<UINT>(std::max<int>(0, std::min<int>(h, textureHeight - y))),
				1, 1
			},    // size
			D3D11_COPY_NO_OVERWRITE_CONC // This is being done from job threads
		};

		if (mapping.Extent.Width && mapping.Extent.Height && mapping.Extent.Depth)
			commandList.GetCopyInterface()->Copy(CRendererResources::s_ptexHDRTarget->GetDevTexture(), CRendererResources::s_ptexSceneTarget->GetDevTexture(), mapping);
	}
}

void CSceneRenderPass::BeginRenderPass(CDeviceCommandListRef RESTRICT_REFERENCE commandList, bool bNearest) const
{
	// Note: Function has to be threadsafe since it can be called from several worker threads


	D3D11_VIEWPORT viewport = GetViewport(bNearest);
	bool bViewportSet = false;

	CDeviceGraphicsCommandInterface* pCommandInterface = commandList.GetGraphicsInterface();
	pCommandInterface->BeginRenderPass(*m_pRenderPass, m_scissorRect);

	if (m_passFlags & ePassFlags_VrProjectionPass)
	{
		bViewportSet = CVrProjectionManager::Instance()->SetRenderingState(commandList, viewport,
			(m_passFlags & ePassFlags_UseVrProjectionState) != 0, (m_passFlags & ePassFlags_RequireVrProjectionConstants) != 0);
	}
	
	if (!bViewportSet)
	{
		pCommandInterface->SetViewports(1, &viewport);
		pCommandInterface->SetScissorRects(1, &m_scissorRect);
	}

	pCommandInterface->SetResourceLayout(m_pResourceLayout.get());
	pCommandInterface->SetResources(EResourceLayoutSlot_PerPassRS, m_pPerPassResourceSet.get());

#if (CRY_RENDERER_DIRECT3D < 120)
	pCommandInterface->SetDepthBias(m_depthConstBias, m_depthSlopeBias, m_depthBiasClamp);
#endif
}

void CSceneRenderPass::EndRenderPass(CDeviceCommandListRef RESTRICT_REFERENCE commandList, bool bNearest) const
{
	// Note: Function has to be threadsafe since it can be called from several worker threads

	CDeviceGraphicsCommandInterface* pCommandInterface = commandList.GetGraphicsInterface();
	pCommandInterface->EndRenderPass(*m_pRenderPass);

#if (CRY_RENDERER_DIRECT3D < 120)
	pCommandInterface->SetDepthBias(0.0f, 0.0f, 0.0f);
#endif

	if (m_passFlags & ePassFlags_UseVrProjectionState)
	{
		CDeviceGraphicsCommandInterface* pCommandInterface = commandList.GetGraphicsInterface();
		CVrProjectionManager::Instance()->RestoreState(commandList);
	}
}

void CSceneRenderPass::BeginExecution()
{
	assert(s_recursionCounter == 0);
	s_recursionCounter += 1;
	
	m_numRenderItemGroups = 0;

	if (gcpRendD3D->GetGraphicsPipeline().GetRenderPassScheduler().IsActive())
		gcpRendD3D->GetGraphicsPipeline().GetRenderPassScheduler().AddPass(this);
}

void CSceneRenderPass::EndExecution()
{
	s_recursionCounter -= 1;
}

inline void DebugDrawRenderResolve(const std::vector<TRect_tpl<uint16>> &ns, const std::size_t count)
{
	// Color table for debug draw
	const auto a = 64;
	ColorB colors[] = {
		ColorB(255, 0, 0, a)    , // Red
		ColorB(0, 255, 0, a)    , // Green
		ColorB(0, 0, 255, a)    , // Blue
		ColorB(255, 255, 0, a)  , // Yellow
		ColorB(255, 0, 255, a)  , // Magenta
		ColorB(0, 255, 255, a)  , // Cyan
		ColorB(128, 0, 0, a)	,
		ColorB(0, 128, 0, a)	,
		ColorB(0, 0, 128, a)	,
		ColorB(128, 128, 0, a)	,
		ColorB(128, 0, 128, a)	,
		ColorB(0, 128, 128, a)	,
		ColorB(192, 192, 192, a),
		ColorB(128, 128, 128, a),
		ColorB(153, 153, 255, a),
		ColorB(153, 51, 102, a)	,
		ColorB(255, 255, 204, a),
		ColorB(204, 255, 255, a),
		ColorB(102, 0, 102, a)	,
		ColorB(255, 128, 128, a),
		ColorB(0, 102, 204, a)	,
		ColorB(204, 204, 255, a),
		ColorB(0, 0, 128, a)	,
		ColorB(255, 0, 255, a)	,
		ColorB(255, 255, 0, a)	,
		ColorB(0, 255, 255, a)	,
		ColorB(128, 0, 128, a)	,
		ColorB(128, 0, 0, a)	,
		ColorB(0, 128, 128, a)	,
		ColorB(0, 0, 255, a)	,
		ColorB(0, 204, 255, a)	,
		ColorB(204, 255, 255, a),
		ColorB(204, 255, 204, a),
		ColorB(255, 255, 153, a),
		ColorB(153, 204, 255, a),
		ColorB(255, 153, 204, a),
		ColorB(204, 153, 255, a),
		ColorB(255, 204, 153, a),
		ColorB(51, 102, 255, a)	,
		ColorB(51, 204, 204, a)	,
		ColorB(153, 204, 0, a)	,
		ColorB(255, 204, 0, a)	,
		ColorB(255, 153, 0, a)	,
		ColorB(255, 102, 0, a)	,
		ColorB(102, 102, 153, a),
		ColorB(150, 150, 150, a),
		ColorB(0, 51, 102, a)	,
		ColorB(51, 153, 102, a)	,
		ColorB(0, 51, 0, a)		,
		ColorB(51, 51, 0, a)	,
		ColorB(153, 51, 0, a)	,
		ColorB(153, 51, 102, a)	,
		ColorB(51, 51, 153, a)	,
		ColorB(51, 51, 51, a)	,
		ColorB(0, 0, 0, a)		,
		ColorB(255, 255, 255, a),
	};

	if (CRendererCVars::CV_r_RefractionPartialResolvesDebug == 2)
	{
		const auto oldAuxFlags = IRenderAuxGeom::GetAux()->GetRenderFlags();
		IRenderAuxGeom::GetAux()->SetRenderFlags(e_AlphaBlended | e_DepthWriteOff | e_DepthTestOff | e_Mode2D);

		for (const auto& n : ns)
		{
			// Render resolve screen-space bounding box
			Vec3 v0 = { static_cast<float>(n.Min.x), static_cast<float>(n.Min.y), .0f };
			Vec3 v1 = { static_cast<float>(n.Max.x), static_cast<float>(n.Min.y), .0f };
			Vec3 v2 = { static_cast<float>(n.Max.x), static_cast<float>(n.Max.y), .0f };
			Vec3 v3 = { static_cast<float>(n.Min.x), static_cast<float>(n.Max.y), .0f };

			const auto c = colors[count % (sizeof(colors) / sizeof(colors[0]))];
			IRenderAuxGeom::GetAux()->DrawQuad(v0, c, v1, c, v2, c, v3, c);
		}

		IRenderAuxGeom::GetAux()->SetRenderFlags(oldAuxFlags);
	}

#if defined(ENABLE_PROFILING_CODE)
	// Write statstics
	SRenderStatistics& stats = SRenderStatistics::Write();
	++stats.m_refractionPartialResolveCount;
	for (const auto& n : ns)
		stats.m_refractionPartialResolvePixelCount += std::max(0, (n.Max.x - n.Min.x) * (n.Max.y - n.Min.y));
#endif
}

const char* RenderListNames[] =
{
	"INVALID",

	"GENERAL",
	"TERRAIN",
	"SHADOWS",
	"DECAL",
	"WATER_VOLUMES",
	"GENERAL",
	"GENERAL",
	"NEAREST",
	"WATER",
	"AFTER_HDRPOSTPROCESS",
	"AFTER_POSTPROCESS",
	"SHADOW_PASS",
	"HALFRES_PARTICLES",
	"PARTICLES_THICKNESS",
	"LENSOPTICS",
	"VOXELIZE",
	"EYE",
	"FOG_VOLUME",
	"NEAREST",
	"GENERAL",
	"NEAREST",
	"DEBUG_HELPER",
	"SKY",

	"PREPROCESS",
	"NON-NEAREST",
	"NEAREST",
	"CUSTOM",
	"HIGHLIGHT",
};

void CSceneRenderPass::DrawRenderItems(CRenderView* pRenderView, ERenderListID renderList, int listStart, int listEnd)
{
	CRY_ASSERT(s_recursionCounter == 1);

	// Completely skip filling of the command list.
	if (CRenderer::CV_r_NoDraw == 2)
		return;

	// Skip nearest objects lists
	const bool bNearest =
		(renderList == EFSLIST_ZPREPASS_NEAREST) ||
		(renderList == EFSLIST_NEAREST_OBJECTS) ||
		(renderList == EFSLIST_FORWARD_OPAQUE_NEAREST) ||
		(renderList == EFSLIST_TRANSP_NEAREST);
	if (bNearest && (CRenderer::CV_r_nodrawnear == 1) && (m_passFlags & CSceneRenderPass::ePassFlags_RenderNearest))
		return;

	// Skip processing if all elements are filtered away
	if (!pRenderView->HasRenderItems(renderList, m_batchIncludeFilter | FB_COMPILED_OBJECT))
		return;

	// Produce valid range of renderable items and skip processing when there are none
	listEnd   = std::min<int>(listEnd, pRenderView->GetRenderItems(renderList).size());
	listStart = std::min<int>(listStart, listEnd);

	if (listStart >= listEnd)
		return;

	CryStackStringT<char, 80> label; label.Format("%s%s (%s)", m_batchIncludeFilter & FB_Z ? "Z " : "", GetLabel(), RenderListNames[pRenderView->GetRecordingRenderList(renderList)]);
	SGraphicsPipelinePassContext passContext(pRenderView, this, m_technique, m_batchIncludeFilter, m_batchExcludeFilter);
	passContext.stageID = m_stageID;
	passContext.passID = m_passID;
	passContext.renderNearest = bNearest && (m_passFlags & CSceneRenderPass::ePassFlags_RenderNearest);
	passContext.renderListId = renderList;
#if defined(ENABLE_PROFILING_CODE)
	passContext.recordListId = pRenderView->GetRecordingRenderList(renderList); // use pseudo-list for stats recording only
#endif
#if defined(DO_RENDERSTATS)
	if (gcpRendD3D->CV_r_stats == 6 || gcpRendD3D->m_pDebugRenderNode || gcpRendD3D->m_bCollectDrawCallsInfoPerNode)
		passContext.pDrawCallInfoPerNode = gcpRendD3D->GetGraphicsPipeline().GetDrawCallInfoPerNode();
	if (gcpRendD3D->m_bCollectDrawCallsInfo)
		passContext.pDrawCallInfoPerMesh = gcpRendD3D->GetGraphicsPipeline().GetDrawCallInfoPerMesh();
#endif

	passContext.groupLabel = label;
	passContext.groupIndex = m_numRenderItemGroups++;
#if defined(ENABLE_SIMPLE_GPU_TIMERS)
	passContext.profilerSectionIndex = gcpRendD3D->m_pPipelineProfiler->InsertMultithreadedSection(passContext.groupLabel.c_str());
#endif

	const bool bTransparent = (renderList == EFSLIST_TRANSP_BW || renderList == EFSLIST_TRANSP_AW || renderList == EFSLIST_TRANSP_NEAREST);
	if (!bTransparent)
		DrawOpaqueRenderItems(passContext, pRenderView, renderList, listStart, listEnd);
	else
		DrawTransparentRenderItems(passContext, pRenderView, renderList, listStart, listEnd);
}

void CSceneRenderPass::DrawOpaqueRenderItems(SGraphicsPipelinePassContext& passContext, CRenderView* pRenderView, ERenderListID renderList, int listStart, int listEnd)
{
	passContext.rendItems = { listStart, listEnd };

	if (gcpRendD3D->GetGraphicsPipeline().GetRenderPassScheduler().IsActive())
	{
		m_passContexts.emplace_back(std::move(passContext));
		return;
	}

	if (!CRenderer::CV_r_NoDraw)
		pRenderView->DrawCompiledRenderItems(passContext);
}

void CSceneRenderPass::DrawTransparentRenderItems(SGraphicsPipelinePassContext& passContext, CRenderView* pRenderView, ERenderListID renderList, int listStart, int listEnd)
{
	std::vector<SGraphicsPipelinePassContext> passes;

	// Wait for the transparent items' refractive passes sort job (if any)
	pRenderView->WaitForOptimizeTransparentRenderItemsResolvesJob();

	if (!pRenderView->HasResolveForList(renderList))
	{
		passContext.rendItems = { listStart, listEnd };

		passes.emplace_back(std::move(passContext));
	}
	else
	{
		if (CRendererCVars::CV_r_RefractionPartialResolveMode == 0)
		{
			// Fast static mode
			// Single fullscreen Resolve the full screen once, before submitting transparent items.

			const auto &vp = pRenderView->GetViewport();

			SGraphicsPipelinePassContext resolvePass = { GraphicsPipelinePassType::resolve, pRenderView, this };
			resolvePass.stageID    = passContext.stageID;
			resolvePass.passID     = passContext.passID;
			resolvePass.groupLabel = passContext.groupLabel;
			resolvePass.groupIndex = passContext.groupIndex;
			resolvePass.resolveScreenBounds.push_back(TRect_tpl<uint16>{ static_cast<uint16>(vp.x), static_cast<uint16>(vp.y), static_cast<uint16>(vp.width), static_cast<uint16>(vp.height) });
#if defined(ENABLE_SIMPLE_GPU_TIMERS)
			resolvePass.profilerSectionIndex = passContext.profilerSectionIndex;
#endif

			if (CRendererCVars::CV_r_RefractionPartialResolvesDebug)
				DebugDrawRenderResolve(resolvePass.resolveScreenBounds, 0);

			passContext.rendItems = { listStart, listEnd };

			passes.emplace_back(std::move(resolvePass));
			passes.emplace_back(std::move(passContext));
		}
		else
		{
			// Segments were percomputed in job "OptimizeTransparentRenderItemsResolvesJob"
			const auto& segments = static_cast<const CRenderView*>(pRenderView)->GetTransparentSegments(renderList);

			std::size_t count = 0;
			for (const auto &s : segments)
			{
				if (s.rendItems.IsEmpty())
					continue;

				if (s.resolveRects.size())
				{
					SGraphicsPipelinePassContext resolvePass = { GraphicsPipelinePassType::resolve, pRenderView, this };
					resolvePass.stageID    = passContext.stageID;
					resolvePass.passID     = passContext.passID;
					resolvePass.groupLabel = passContext.groupLabel;
					resolvePass.groupIndex = passContext.groupIndex;
					resolvePass.resolveScreenBounds = s.resolveRects;
#if defined(ENABLE_SIMPLE_GPU_TIMERS)
					resolvePass.profilerSectionIndex = passContext.profilerSectionIndex;
#endif

					if (CRendererCVars::CV_r_RefractionPartialResolvesDebug)
						DebugDrawRenderResolve(s.resolveRects, count++);

					passes.emplace_back(std::move(resolvePass));
				}

				passContext.rendItems = s.rendItems;

				passes.push_back(passContext);

			}
		}
	}

	if (gcpRendD3D->GetGraphicsPipeline().GetRenderPassScheduler().IsActive())
	{
		m_passContexts.reserve(m_passContexts.size() + passes.size());
		std::copy(passes.begin(), passes.end(), std::back_inserter(m_passContexts));

		return;
	}

	if (!CRenderer::CV_r_NoDraw)
	{
		for (const auto &pass : passes)
			pRenderView->DrawCompiledRenderItems(pass);
	}
}

void CSceneRenderPass::Execute()
{
	for (const auto& passContext : m_passContexts)
		passContext.pRenderView->DrawCompiledRenderItems(passContext);
}
