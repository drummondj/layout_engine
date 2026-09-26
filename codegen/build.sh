#!/bin/bash

export PYTHONPATH=$(pwd)

poetry run pyinstaller --onefile --distpath bin --name codegen codegen/cli.py
