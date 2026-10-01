// *****************************************************************************
// *****************************************************************************
// Copyright 2013 - 2020, Cadence Design Systems
// 
// This  file  is  part  of  the  Cadence  LEF/DEF  Open   Source
// Distribution,  Product Version 5.8. 
// 
// Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
// 
//        http://www.apache.org/licenses/LICENSE-2.0
// 
//    Unless required by applicable law or agreed to in writing, software
//    distributed under the License is distributed on an "AS IS" BASIS,
//    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
//    implied. See the License for the specific language governing
//    permissions and limitations under the License.
// 
// For updates, support, or to become part of the LEF/DEF Community,
// check www.openeda.org for details.
// 
//  $Author: icftcm $
//  $Revision: #10 $
//  $Date: 2023/06/26 $
//  $State:  $
// *****************************************************************************
// *****************************************************************************

#ifndef defiPropType_h
#define defiPropType_h

#include "defiKRDefs.hpp"
#include <stdio.h>

BEGIN_LEFDEF_PARSER_NAMESPACE



// *****************************************************************************
// This class is used to cache the names and types of all the DEF properties
// defined for a single objectType (eg. COMPONENT, NET, etc).
// *****************************************************************************
class defiPropType {
public:
  defiPropType();
  void Init();

  void Destroy();
  ~defiPropType();

  void setPropType(const char* name, char type);
  void Clear();

  char propType(const char* name) const;
  void bumpProps();

protected:
  int    numProperties_;
  int    propertiesAllocated_;
  char** propNames_;      // Array of property names.
  char*  propTypes_;      // Array of property types.
                          // 'R' == "REAL", 'I' == "INTEGER", 'S' == STRING.
};


END_LEFDEF_PARSER_NAMESPACE

USE_LEFDEF_PARSER_NAMESPACE

#endif
