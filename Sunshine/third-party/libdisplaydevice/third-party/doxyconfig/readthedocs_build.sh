#!/usr/bin/env bash
set -e

function setup_conda_env {
  echo "Setting up conda environment"
  local environment_file="third-party/doxyconfig/environment.yml"

  if [[ "${DOXYCONFIG_DIR}" == "." ]]; then
    mkdir -p third-party/doxyconfig
    cp environment.yml $environment_file
    cp -r doxygen-awesome-css third-party/doxyconfig/
  fi

  echo "cat $environment_file"
  cat $environment_file

  echo "conda env create --quiet --name ${READTHEDOCS_VERSION} --file $environment_file"
  conda env create --quiet --name "${READTHEDOCS_VERSION}" --file "$environment_file"
  return 0
}

function install_icons {
  echo "Downloading LizardByte graphics"
  wget "https://raw.githubusercontent.com/LizardByte/.github/master/branding/logos/favicon.ico" \
    -O "${READTHEDOCS_OUTPUT}lizardbyte.ico"
  wget "https://raw.githubusercontent.com/LizardByte/.github/master/branding/logos/logo-128x128.png" \
    -O "${READTHEDOCS_OUTPUT}lizardbyte.png"
  return 0
}

function install_node_modules {
  echo "Creating output directories"
  mkdir -p "${READTHEDOCS_OUTPUT}html/assets/fontawesome/css"
  mkdir -p "${READTHEDOCS_OUTPUT}html/assets/fontawesome/js"
  mkdir -p "${READTHEDOCS_OUTPUT}html/assets/shared-web"

  echo "Installing node modules"
  pushd "${DOXYCONFIG_DIR}"
  npm install --ignore-scripts
  popd

  echo "Copying FontAwesome files"
  cp "${DOXYCONFIG_DIR}/node_modules/@fortawesome/fontawesome-free/css/all.min.css" \
    "${READTHEDOCS_OUTPUT}html/assets/fontawesome/css"
  cp "${DOXYCONFIG_DIR}/node_modules/@fortawesome/fontawesome-free/js/all.min.js" \
    "${READTHEDOCS_OUTPUT}html/assets/fontawesome/js"
  cp -r "${DOXYCONFIG_DIR}/node_modules/@fortawesome/fontawesome-free/webfonts" \
    "${READTHEDOCS_OUTPUT}html/assets/fontawesome/"

  echo "Copying shared-web files"
  cp "${DOXYCONFIG_DIR}/node_modules/@lizardbyte/shared-web/dist/crowdin.js" \
    "${READTHEDOCS_OUTPUT}html/assets/shared-web/"
  cp "${DOXYCONFIG_DIR}/node_modules/@lizardbyte/shared-web/dist/crowdin-doxygen-css.css" \
    "${READTHEDOCS_OUTPUT}html/assets/shared-web/"
  return 0
}

function merge_doxyconfigs {
  local docs_dir="./docs/"
  echo "Merging doxygen configs"
  cp "${DOXYCONFIG_DIR}/doxyconfig-Doxyfile" "${docs_dir}"
  cp "${DOXYCONFIG_DIR}/doxyconfig-header.html" "${docs_dir}"
  cp "${DOXYCONFIG_DIR}/doxyconfig.css" "${docs_dir}"
  cp "${DOXYCONFIG_DIR}/doxyconfig-readthedocs-search.js" "${docs_dir}"
  cat "${docs_dir}Doxyfile" >> "${docs_dir}doxyconfig-Doxyfile"
  return 0
}

function build_docs {
  echo "Building docs"
  pushd docs
  doxygen doxyconfig-Doxyfile
  popd
  return 0
}

setup_conda_env
install_node_modules
install_icons
merge_doxyconfigs
build_docs
