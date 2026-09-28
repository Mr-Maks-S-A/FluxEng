#pragma once
/**
 * @file RendererSystem.hpp
 * @brief Общий заголовок модуля: подключает весь публичный API рендера.
 */

#include <RendererSystem/Core/Color.hpp>
#include <RendererSystem/Core/Error.hpp>
#include <RendererSystem/Core/Geometry.hpp>
#include <RendererSystem/Core/Handles.hpp>
#include <RendererSystem/Core/Image.hpp>

#include <RendererSystem/Scene/Camera2D.hpp>

#include <RendererSystem/Batch/SpriteBatch.hpp>

#include <RendererSystem/Animation/Animation.hpp>

#include <RendererSystem/GL/Framebuffer.hpp>
#include <RendererSystem/GL/Shader.hpp>
#include <RendererSystem/GL/Texture.hpp>

#include <RendererSystem/Renderer2D.hpp>

/**
 * @namespace RendererSystem
 * @brief 2D-рендер FluxEng: CPU-ядро (батчи, камера, анимации, изображения) и OpenGL-бэкенд.
 */

/**
 * @namespace RendererSystem::GL
 * @brief RAII-обёртки над объектами OpenGL 3.3.
 */
