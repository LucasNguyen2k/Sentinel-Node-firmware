# Build/test agent for the Sentinel Node pipeline (stages L1-L3, L4 emulator, mutation, traceability).
FROM ubuntu:24.04
ARG ARDUINO_CLI_VERSION=1.3.1
ARG AMEBA_CORE_VERSION=4.1.0
ARG AWOT_COMMIT=7c508105d2bc9ad390e5ceea6f852f9457b28924
ENV DEBIAN_FRONTEND=noninteractive \
    ARDUINO_DIRECTORIES_DATA=/opt/arduino15 \
    ARDUINO_DIRECTORIES_USER=/opt/arduino \
    ARDUINO_BUILD_CACHE_PATH=/opt/arduino-cache \
    ARDUINO_BOARD_MANAGER_ADDITIONAL_URLS=https://github.com/Ameba-AIoT/ameba-arduino-pro2/raw/main/Arduino_package/package_realtek_amebapro2_index.json \
    PIP_DISABLE_PIP_VERSION_CHECK=1
RUN apt-get update && apt-get install -y --no-install-recommends \
        python3 python3-pip g++ make git curl ca-certificates bzip2 xz-utils zip unzip cppcheck pyflakes3 \
    && rm -rf /var/lib/apt/lists/*
RUN curl -fsSL https://github.com/arduino/arduino-cli/releases/download/v${ARDUINO_CLI_VERSION}/arduino-cli_${ARDUINO_CLI_VERSION}_Linux_64bit.tar.gz \
        | tar -xz -C /usr/local/bin arduino-cli \
    && arduino-cli core update-index \
    && arduino-cli core install realtek:AmebaPro2@${AMEBA_CORE_VERSION}
RUN mkdir -p /opt/arduino/libraries && cd /opt/arduino/libraries \
    && git clone https://github.com/lasselukkari/aWOT && git -C aWOT checkout ${AWOT_COMMIT}
COPY server/requirements.txt /tmp/server-requirements.txt
COPY tests/requirements.txt /tmp/test-requirements.txt
RUN pip3 install --break-system-packages -r /tmp/server-requirements.txt -r /tmp/test-requirements.txt
# Jenkins runs the container with its own UID: the toolchain folders must be writable for it
RUN mkdir -p /opt/arduino-cache && chmod -R a+rwX /opt/arduino15 /opt/arduino /opt/arduino-cache
