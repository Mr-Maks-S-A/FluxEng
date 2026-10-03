#!/usr/bin/env bash
# Проверка «у каждого модуля есть описание, примеры использования и тесты, и это не бумажная формальность».
#
# Для каждого Modules/<Имя>:
#   1. есть README.md или docs/mainpage.md;
#   2. есть examples/*.cpp и examples/CMakeLists.txt, и каждый пример в него включён (значит, собирается и запускается как тест);
#   3. каждый пример начинается с doc-комментария `@example` или `@file` (что он показывает);
#   4. есть tests/ с кодом;
#   5. каждый публичный заголовок имеет `@file`-описание.
# Сборка и запуск примеров проверяются CTest: `ctest -L example`.
set -u
root="$(cd "$(dirname "$0")/.." && pwd)"
fail=0
note() { echo "  - $1"; fail=1; }

for dir in "${root}"/Modules/*/; do
    name="$(basename "${dir}")"
    problems=()
    [[ -f "${dir}README.md" || -f "${dir}docs/mainpage.md" ]] || problems+=("нет README.md / docs/mainpage.md")
    if compgen -G "${dir}examples/*.cpp" > /dev/null; then
        [[ -f "${dir}examples/CMakeLists.txt" ]] || problems+=("нет examples/CMakeLists.txt")
        for example in "${dir}"examples/[0-9]*.cpp; do
            base="$(basename "${example}" .cpp)"
            grep -q "${base}" "${dir}examples/CMakeLists.txt" 2>/dev/null || problems+=("пример ${base} не включён в examples/CMakeLists.txt")
            head -12 "${example}" | grep -qE "@example|@file" || problems+=("у примера ${base} нет doc-комментария в начале")
        done
    else
        problems+=("нет примеров (examples/*.cpp)")
    fi
    compgen -G "${dir}tests/*.cpp" > /dev/null || problems+=("нет тестов (tests/*.cpp)")
    while IFS= read -r header; do
        grep -q "@file" "${header}" || problems+=("заголовок без @file: ${header#"${dir}"}")
    done < <(find "${dir}src/include" -name '*.hpp' 2>/dev/null)

    if ((${#problems[@]})); then
        echo "${name}:"
        for p in "${problems[@]}"; do note "${p}"; done
    else
        echo "${name}: ok ($(ls "${dir}"examples/[0-9]*.cpp | wc -l) примеров)"
    fi
done
exit "${fail}"
