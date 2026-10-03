#!/usr/bin/env bash
# Пересоздаёт эталонные записи (golden) для проверки детерминизма. Запускать только если поведение симуляции
# изменено НАМЕРЕННО (физика, цены рун, генерация мира); результат коммитится вместе с изменением.
#
# Использование: tools/update_golden.sh [каталог-сборки]
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-${root}/build}"
cmake --build "${build}" --target SpellSimTests -j
FLUX_UPDATE_GOLDEN=1 "${build}/bin/SpellSimTests" -tc="Golden*" || true
echo "Готово. Проверьте diff:  git status Modules/SpellSim/tests/golden"
