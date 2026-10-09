#!/usr/bin/env bash
# Проверка самостоятельности модулей: каждый модуль собирается и тестируется
# отдельно от движка (cmake -S Modules/<Name>). Если модуль незаметно начал
# зависеть от чего-то, что подключает только корень движка, здесь это упадёт.
#
# Использование: tools/check_modules.sh [каталог-сборки] [доп. аргументы cmake...]
#   tools/check_modules.sh build-modules -DCMAKE_CXX_COMPILER=g++-14
# Тесты с окном требуют дисплей; без него запускайте через xvfb-run.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
build_root="${1:-${root}/build-modules}"
shift || true

mkdir -p "${build_root}"

modules=(MemorySystem JobSystem EventSystem RuntimeSystem AssetSystem NetSystem InputSystem WindowSystem ECSSystem RendererSystem Core)
failed=()

for module in "${modules[@]}"; do
    [[ -d "${root}/Modules/${module}" ]] || continue
    echo "==== ${module}"
    dir="${build_root}/${module}"
    if cmake -S "${root}/Modules/${module}" -B "${dir}" -DCMAKE_BUILD_TYPE=RelWithDebInfo "$@" > "${dir}.configure.log" 2>&1 \
        && cmake --build "${dir}" --parallel > "${dir}.build.log" 2>&1 \
        && ctest --test-dir "${dir}" --output-on-failure > "${dir}.test.log" 2>&1; then
        echo "     ok ($(grep -Eo '[0-9]+% tests passed(, [0-9]+ tests? failed)? out of [0-9]+' "${dir}.test.log" || echo 'no tests'))"
    else
        echo "     FAILED — см. ${dir}.*.log"
        failed+=("${module}")
    fi
done

if ((${#failed[@]})); then
    echo "Не прошли: ${failed[*]}"
    exit 1
fi
echo "Все модули собираются и проходят тесты по отдельности."
