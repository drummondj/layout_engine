#!/bin/bash

export PYTHONPATH=$(pwd)
# sphinx-build sphinx docs

poetry run pyinstaller --onefile --distpath bin --name codegen codegen/cli.py
