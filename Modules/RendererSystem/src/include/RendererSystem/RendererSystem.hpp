#pragma once
/**
 * @file RendererSystem.hpp
 * @brief Общий заголовок модуля: подключает весь публичный API рендера.
 */

#include <RendererSystem/Core/Color.hpp>
#include <RendererSystem/Core/Error.hpp>
#include <RendererSystem/Core/Geometry.hpp>
#include <RendererSystem/Core/Geometry3D.hpp>
#include <RendererSystem/Core/Handles.hpp>
#include <RendererSystem/Core/Image.hpp>
#include <RendererSystem/Core/Procedural.hpp>

#include <RendererSystem/Scene/Camera2D.hpp>
#include <RendererSystem/Scene/Camera3D.hpp>

#include <RendererSystem/Mesh/MeshData.hpp>

#include <RendererSystem/Text/Font.hpp>

#include <RendererSystem/Batch/SpriteBatch.hpp>

#include <RendererSystem/Animation/Animation.hpp>

#include <RendererSystem/RHI/Device.hpp>
#include <RendererSystem/RHI/Resources.hpp>
#include <RendererSystem/RHI/Types.hpp>

#include <RendererSystem/Renderer2D.hpp>
#include <RendererSystem/Renderer3D.hpp>

/**
 * @namespace RendererSystem
 * @brief Рендер FluxEng: CPU-ядро (батчи, камеры, сетки, шрифты, изображения, шум) и OpenGL-бэкенд (2D и 3D).
 */

/**
 * @namespace RendererSystem::Procedural
 * @brief Шум Перлина (stb_perlin) и процедурные изображения.
 */

/**
 * @namespace RendererSystem::RHI
 * @brief Граница с графическим API: устройство (OpenGL или Vulkan), ручки ресурсов, конвейеры, вызовы рисования.
 */
