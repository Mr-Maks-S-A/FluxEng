// Renderer/Texture.h
#pragma once

#include <string>
#include <cstdint>

namespace Renderer {

    /**
     * @brief Класс для управления текстурами OpenGL
     * Загружает изображения и создает OpenGL текстуры
     */
    class Texture {
    private:
        unsigned int m_textureID;
        int m_width;
        int m_height;
        std::string m_filePath;

    public:
        Texture();
        ~Texture();

        // Запрещаем копирование
        Texture(const Texture&) = delete;
        Texture& operator=(const Texture&) = delete;

        // Разрешаем перемещение
        Texture(Texture&& other) noexcept;
        Texture& operator=(Texture&& other) noexcept;

        /**
         * @brief Загрузка текстуры из файла
         * @param filePath Путь к изображению (PNG, JPG, BMP и т.д.)
         * @return true если успешно
         */
        bool loadFromFile(const std::string& filePath);

        /**
         * @brief Создание пустой текстуры заданного размера
         * @param width Ширина
         * @param height Высота
         * @param data Данные пикселей (nullptr для пустой)
         */
        void create(int width, int height, const uint8_t* data = nullptr);

        /**
         * @brief Активация текстуры
         * @param textureUnit Номер текстурного юнита (0-31)
         */
        void bind(int textureUnit = 0) const;

        /**
         * @brief Деактивация
         */
        void unbind() const;

        /**
         * @brief Получение ID текстуры OpenGL
         */
        unsigned int getTextureID() const { return m_textureID; }

        /**
         * @brief Получение ширины текстуры
         */
        int getWidth() const { return m_width; }

        /**
         * @brief Получение высоты текстуры
         */
        int getHeight() const { return m_height; }

        /**
         * @brief Проверка, загружена ли текстура
         */
        bool isValid() const { return m_textureID != 0; }
    };

} // namespace Renderer