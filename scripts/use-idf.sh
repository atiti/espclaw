#!/bin/zsh

export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$(cd "$(dirname "${(%):-%N}")/.." && pwd)/.deps/.espressif}"
export IDF_PATH="${IDF_PATH:-$(cd "$(dirname "${(%):-%N}")/.." && pwd)/.deps/esp-idf-v5.5.2}"

if [[ -z "$IDF_PYTHON_ENV_PATH" ]]; then
  setopt local_options null_glob
  typeset -a python_env_candidates=("$IDF_TOOLS_PATH"/python_env/idf5.5_py3.*_env)

  if (( ${#python_env_candidates[@]} > 0 )); then
    export IDF_PYTHON_ENV_PATH="${python_env_candidates[1]}"
  fi
fi

if [[ -n "$IDF_PYTHON_ENV_PATH" && -d "$IDF_PYTHON_ENV_PATH/bin" ]]; then
  export PATH="$IDF_PYTHON_ENV_PATH/bin:$PATH"
fi

if [[ ! -f "$IDF_PATH/export.sh" ]]; then
  echo "ESP-IDF export script not found at $IDF_PATH/export.sh" >&2
  return 1 2>/dev/null || exit 1
fi

source "$IDF_PATH/export.sh"
