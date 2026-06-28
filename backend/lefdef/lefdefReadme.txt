This directory contains the the 6.0 version of Library Exchange Format (LEF) and Design
Exchange Format (DEF). LEF and DEF are distributed free of charge. See the
License.txt file in the LEF and DEF modules for conditions attached to the use of this
software, and the copying or distribution of it.

The Make programs for some systems may not be able to process the Makefile for the
LEF/DEF utilities. If your system's native Make program returns errors when building
LEF/DEF, use GNU Make, or change the Makefiles so that they work on your system.

Documentation

Documentation for the LEF and DEF application programming interfaces can be found in
the lefdefrefDoc tar kit.

Bug Reporting

Please report issues with this version of the LEF/DEF parser directly to Cadence.
Email jgrad@cadence.com

Platforms Supported

The LEF/DEF utilities have been tested on Linux RH 7.4.

Installation

The following information explains how to compile the LEF and DEF packages.

Installing LEF 

To install LEF, do the following:

1. Change directories (cd) to the lef subdirectory containing the package's source code.
2. To compile and build the package, type the following command:
gmake

3. To set DEBUG with OPTIMIZE_FLAG, type the following command:
gmake release

4. To install headers before compiling lefrw lefdiff lefwrite, type the following
command:
gmake installhdrs

5. Optionally, to run the tests that come with the package, type the following command:
./gmake test

NOTE: To remove the program binaries and object files from the source code directory,
type the following command:

gmake clean


Installing DEF

To install DEF, do the following:

1. Change directories (cd) to the def subdirectory containing the package's source code.
2. To compile and build the package, type the following command:
gmake

3. To set DEBUG with OPTIMIZE_FLAG, type the following command:
gmake release

4. To install headers before compiling defrw defdiff defwrite, type the following command:
gmake installhdrs

5. Optionally, to run the tests that come with the package, type the following command:
./gmake test

NOTE: To remove the program binaries and object files from the source code directory,
type the following command:

gmake clean

