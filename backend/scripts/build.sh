#!/usr/bin/env bash

cmake --build build -j3 && cmake --build build_release --target api pipelines io -j3