# ==============================================================================
# PROJECT: Fluxborn Engine
# MODULE:  STB Library Profile (Image, Resize, Audio)
# ROLE:    Fast and reliable media loading for Voxel Assets
# ==============================================================================
# --- 2. МОДУЛЬНАЯ КОНФИГУРАЦИЯ ---
# Эти флаги управляют тем, какие части STB будут скомпилированы в твоем stb/CMakeLists.txt
set(STB_IMAGE_IMPLEMENTATION    ON CACHE BOOL "" FORCE)
set(STB_IMAGE_RESIZE_IMPL       ON CACHE BOOL "" FORCE)
set(STB_TRUETYPE_IMPLEMENTATION ON CACHE BOOL "" FORCE)
set(STB_VORBIS_HEADER_ONLY      OFF CACHE BOOL "" FORCE)

# --- 3. ФИНАЛЬНЫЙ СТАТУС ---
message(STATUS "[STB Config] Profile: Image, Resize, TrueType, Vorbis (Safe Errors).")
