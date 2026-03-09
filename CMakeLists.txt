# ESP-IDF project for Slack Reactions E-Paper Display
# Migrated from Arduino framework for light sleep with WiFi DTIM support
#
# Build for LilyGo T5 (ESP32):
#   idf.py -DIDF_TARGET=esp32 build
#
# Build for custom PCB (ESP32-S3):
#   idf.py -DIDF_TARGET=esp32s3 build
#
# Flash and monitor:
#   idf.py -p /dev/cu.usbserial-* flash monitor

cmake_minimum_required(VERSION 3.16)

# Pull in ESP-IDF build system
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(slack_reactions_epaper)
