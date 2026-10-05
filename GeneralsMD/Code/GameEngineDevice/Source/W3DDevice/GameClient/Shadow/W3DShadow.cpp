/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////


// FILE: W3DShadow.cpp ///////////////////////////////////////////////////////////
//
// Real time shadow representations
//
// Author: Mark Wilczynski, February 2002
//
//

// USER INCLUDES //////////////////////////////////////////////////////////////
#include "always.h"
#include "GameClient/View.h"
#include "WW3D2/camera.h"
#include "WW3D2/light.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/hlod.h"
#include "WW3D2/mesh.h"
#include "WW3D2/meshmdl.h"
#include "Lib/BaseType.h"
#include "W3DDevice/GameClient/HeightMap.h"
#include "d3dx8math.h"
#include "Common/GlobalData.h"
#include "W3DDevice/GameClient/W3DVolumetricShadow.h"
#include "W3DDevice/GameClient/W3DProjectedShadow.h"
#include "W3DDevice/GameClient/W3DShadow.h"
#include "WW3D2/statistics.h"
#include "Common/Debug.h"
#include "Common/PerfTimer.h"

#define SUN_DISTANCE_FROM_GROUND	10000.0f	//distance of sun (our only light source).

// Global Variables and Functions /////////////////////////////////////////////
W3DShadowManager *TheW3DShadowManager=nullptr;
const FrustumClass *shadowCameraFrustum;

Vector3 LightPosWorld[ MAX_SHADOW_LIGHTS ] =
{

	Vector3( 94.0161f, 50.499f, 200.0f)
};

void PrepareShadows()
{
	if (TheW3DProjectedShadowManager)
		TheW3DProjectedShadowManager->prepareShadows();
}

//DECLARE_PERF_TIMER(shadowsRender)
void DoShadows(RenderInfoClass & rinfo, Bool stencilPass)
{
	//USE_PERF_TIMER(shadowsRender)
	shadowCameraFrustum=&rinfo.Camera.Get_Frustum();
	Int projectionCount=0;

	//Projected shadows render first because they may fill the stencil buffer
	//which will be used by the shadow volumes
	if (stencilPass == FALSE && TheGlobalData->m_useShadowDecals &&
		TheW3DProjectedShadowManager)
	{
		if (TheW3DShadowManager->isShadowScene())
			projectionCount=TheW3DProjectedShadowManager->renderShadows(rinfo);
	}

	if (stencilPass == TRUE && TheGlobalData->m_useShadowVolumes &&
		TheW3DVolumetricShadowManager)
	{

//		TheW3DShadowManager->loadTerrainShadows();

			//This function gets called many times by the W3D renderer
			//so we use this flag to make sure shadows rendered only once per frame.
			if (TheW3DShadowManager->isShadowScene())
				TheW3DVolumetricShadowManager->renderShadows(projectionCount);
	}
	if (TheW3DShadowManager && stencilPass)	//reset so no more shadow processing this frame.
		TheW3DShadowManager->queueShadows(FALSE);

}

W3DShadowManager::W3DShadowManager()
{
	DEBUG_ASSERTCRASH(TheW3DVolumetricShadowManager == nullptr && TheW3DProjectedShadowManager == nullptr,
		("Creating new shadow managers without deleting old ones"));

	m_shadowColor = 0x7fa0a0a0;
	m_isShadowScene = FALSE;
	m_stencilShadowMask = 0;	//all bits can be used for storing shadows.

	Vector3 lightRay(-TheGlobalData->m_terrainLightPos[0].x,
		-TheGlobalData->m_terrainLightPos[0].y, -TheGlobalData->m_terrainLightPos[0].z);
	lightRay.Normalize();

	LightPosWorld[0]=lightRay*SUN_DISTANCE_FROM_GROUND;

	TheW3DVolumetricShadowManager = NEW W3DVolumetricShadowManager;
	TheProjectedShadowManager = TheW3DProjectedShadowManager = NEW W3DProjectedShadowManager;
}

W3DShadowManager::~W3DShadowManager()
{
	delete TheW3DVolumetricShadowManager;
	TheW3DVolumetricShadowManager = nullptr;
	delete TheW3DProjectedShadowManager;
	TheProjectedShadowManager = TheW3DProjectedShadowManager = nullptr;
}

/** Do one-time initilalization of shadow systems that need to be
active for full duration of game*/
Bool W3DShadowManager::init()
{
	Bool result=TRUE;

	// iOS performance profile: initialize only the shadow path that is actually
	// enabled. Projected shadows remain enabled and are used as the lightweight
	// fallback when volumetric/stencil shadows are disabled.
	if (TheGlobalData->m_useShadowVolumes &&
		TheW3DVolumetricShadowManager && TheW3DVolumetricShadowManager->init())
	{
		if (TheW3DVolumetricShadowManager->ReAcquireResources())
			result = TRUE;
	}

	if (TheGlobalData->m_useShadowDecals &&
		TheW3DProjectedShadowManager && TheW3DProjectedShadowManager->init())
	{
		if (TheW3DProjectedShadowManager->ReAcquireResources())
			result = TRUE;
	}

	return result;
}

/** Do per-map reset.  This frees up shadows from all objects since
they may not exist on the next map*/
void W3DShadowManager::Reset()
{

	if (TheW3DVolumetricShadowManager)
		TheW3DVolumetricShadowManager->reset();
	if (TheW3DProjectedShadowManager)
		TheW3DProjectedShadowManager->reset();
}

Bool W3DShadowManager::ReAcquireResources()
{
	Bool result = TRUE;

	if (TheW3DVolumetricShadowManager && !TheW3DVolumetricShadowManager->ReAcquireResources())
		result = FALSE;
	if (TheW3DProjectedShadowManager && !TheW3DProjectedShadowManager->ReAcquireResources())
		result = FALSE;

	return result;
}

void W3DShadowManager::ReleaseResources()
{
	if (TheW3DVolumetricShadowManager)
		TheW3DVolumetricShadowManager->ReleaseResources();
	if (TheW3DProjectedShadowManager)
		TheW3DProjectedShadowManager->ReleaseResources();
}

Shadow *W3DShadowManager::addShadow( RenderObjClass *robj, Shadow::ShadowTypeInfo *shadowInfo, Drawable *draw)
{
	ShadowType type = SHADOW_VOLUME;

	if (shadowInfo)
		type = shadowInfo->m_type;

	// iOS/DXVK compatibility:
	// The legacy stencil-volume path can report a usable stencil surface while
	// still producing no visible shadow on Metal/Vulkan. Prefer the generated
	// projected path on iOS. The UI's 3D/2D shadow switches still control whether
	// the shadow system is enabled, but the backend uses the reliable projected
	// implementation so units/buildings actually cast a visible shadow.
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	if ((type & SHADOW_VOLUME) && TheGlobalData->m_useShadowDecals &&
		TheW3DProjectedShadowManager)
	{
		// iOS/DXVK: use the retail shadow blob first.  It is a cheap, reliable
		// terrain decal and does not depend on render-to-texture/stencil support.
		// If shadow.tga is unavailable, fall back to the generated projected path.
		Shadow::ShadowTypeInfo decalInfo;
		if (shadowInfo)
			decalInfo = *shadowInfo;
		decalInfo.m_type = SHADOW_DECAL;
		decalInfo.m_ShadowName[0] = '\0';
		// Retail shadow.tga is static on iOS; avoid per-frame dynamic-shadow work
        // for every unit/building in large armies.
        decalInfo.allowUpdates = FALSE;

		Shadow *decalShadow =
			(Shadow *)TheW3DProjectedShadowManager->addShadow(robj, &decalInfo, draw);
		if (decalShadow)
			return decalShadow;

		Shadow::ShadowTypeInfo projectedInfo;
		if (shadowInfo)
			projectedInfo = *shadowInfo;
		projectedInfo.m_type = SHADOW_PROJECTION;
		projectedInfo.m_ShadowName[0] = '\0';
		// Generated projection is only a compatibility fallback. Keep the
        // shadow texture static and update projection when the caster moves.
        projectedInfo.allowUpdates = FALSE;

		Shadow *projectedShadow =
			(Shadow *)TheW3DProjectedShadowManager->addShadow(robj, &projectedInfo, draw);
		if (projectedShadow)
			return projectedShadow;
	}
#endif

	// Prefer the original volumetric path where it is known to be usable.
	if ((type & SHADOW_VOLUME) && TheGlobalData->m_useShadowVolumes)
	{
		if (TheW3DVolumetricShadowManager)
		{
			Shadow *volumeShadow =
				(Shadow *)TheW3DVolumetricShadowManager->addShadow(robj, shadowInfo, draw);
			if (volumeShadow)
				return volumeShadow;
		}
	}

	if (TheGlobalData->m_useShadowDecals && TheW3DProjectedShadowManager)
	{
		if (type & (SHADOW_PROJECTION | SHADOW_DYNAMIC_PROJECTION | SHADOW_DECAL |
			SHADOW_ALPHA_DECAL | SHADOW_ADDITIVE_DECAL))
			return (Shadow *)TheW3DProjectedShadowManager->addShadow(robj, shadowInfo, draw);

		// Final fallback for normal SHADOW_VOLUME requests: generate a projected
		// shadow if the stencil/volume implementation cannot create one.
		if (type & SHADOW_VOLUME)
		{
			Shadow::ShadowTypeInfo fallbackInfo;
			if (shadowInfo)
				fallbackInfo = *shadowInfo;
			fallbackInfo.m_type = SHADOW_PROJECTION;
			fallbackInfo.m_ShadowName[0] = '\0';
			fallbackInfo.allowUpdates = TRUE;

			return (Shadow *)TheW3DProjectedShadowManager->addShadow(
				robj, &fallbackInfo, draw);
		}
	}

	return nullptr;
}

void W3DShadowManager::removeShadow(Shadow *shadow)
{
	shadow->release();
}

void W3DShadowManager::removeAllShadows()
{
	if (TheW3DVolumetricShadowManager)
		TheW3DVolumetricShadowManager->removeAllShadows();
	if (TheW3DProjectedShadowManager)
		TheW3DProjectedShadowManager->removeAllShadows();
}

/**Force update of all shadows even when light source and object have not moved*/
void W3DShadowManager::invalidateCachedLightPositions()
{
	if (TheW3DVolumetricShadowManager)
		TheW3DVolumetricShadowManager->invalidateCachedLightPositions();
	if (TheW3DProjectedShadowManager)
		TheW3DProjectedShadowManager->invalidateCachedLightPositions();
}

Vector3 &W3DShadowManager::getLightPosWorld(Int lightIndex)
{
	return LightPosWorld[lightIndex];
}

void W3DShadowManager::setLightPosition(Int lightIndex, Real x, Real y, Real z)
{
	if (lightIndex != 0)
		return;	///@todo: Add support for multiple lights

	LightPosWorld[lightIndex]=Vector3(x,y,z);
}

void W3DShadowManager::setTimeOfDay(TimeOfDay tod)
{
	//Ray to light source
	const GlobalData::TerrainLighting *ol=&TheGlobalData->m_terrainObjectsLighting[tod][0];

	Vector3 lightRay(-ol->lightPos.x,-ol->lightPos.y,-ol->lightPos.z);

	lightRay.Normalize();
	lightRay *= SUN_DISTANCE_FROM_GROUND;

	setLightPosition(0, lightRay.X, lightRay.Y, lightRay.Z);
}
