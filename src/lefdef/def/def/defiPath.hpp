// *****************************************************************************
// *****************************************************************************
// Copyright 2013-2020, Cadence Design Systems
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
//  $Revision: #13 $
//  $Date: 2023/08/16 $
//  $State:  $
// *****************************************************************************
// *****************************************************************************

#ifndef defiPath_h
#define defiPath_h

#include <stdio.h>
#include "defiKRDefs.hpp"
#include "defiProp.hpp"

BEGIN_LEFDEF_PARSER_NAMESPACE

// TX_DIR:TRANSLATION ON

class defrData;
class defrProps;
class defrString;

struct defiPnt {
  int x;
  int y;
  int ext;
};

// 5.4.1 1-D & 2-D Arrays of Vias in SPECIALNET Section
struct defiViaData {
  int numX;
  int numY;
  int stepX;
  int stepY;
};

struct defiViaRect {
  int deltaX1;
  int deltaY1;
  int deltaX2;
  int deltaY2;
};

// value returned by the next() routine.
enum defiPath_e {
  DEFIPATH_DONE = 0,
  DEFIPATH_LAYER,
  DEFIPATH_VIA,
  DEFIPATH_VIAROTATION,
  DEFIPATH_WIDTH,
  DEFIPATH_POINT,
  DEFIPATH_FLUSHPOINT,
  DEFIPATH_TAPER,
  DEFIPATH_SHAPE,
  DEFIPATH_STYLE,
  DEFIPATH_TAPERRULE,
  DEFIPATH_VIADATA,
  DEFIPATH_RECT,
  DEFIPATH_VIRTUALPOINT,
  DEFIPATH_MASK,
  DEFIPATH_VIAMASK
  } ;


class defiPath {
public:
  defiPath(defrData *data);
  // This is 'data ownership transfer' constructor.
  defiPath(defiPath *defiPathRef);

  void Init();

  void Destroy();
  ~defiPath();

  void clear();
  void reverseOrder();

  // Next-based or prev-based traversal of path elements using the member
  // 'pointer_' (not thread safe).
  void initTraverse() const;   // Initialize the traverse.
  void initTraverseBackwards() const;   // Initialize the traverse in reverse.
  int next() const;            // Get the next element.
  int prev() const;            // Get the next element in reverse.

  // Index-based traversal of path elements (thread safe).
  int getNumElements() const;
  int getType(int index) const;

  const char* getLayer() const;
  const char* getLayer(int index) const;

  const char* getTaperRule() const; // Get the rule.
  const char* getTaperRule(int index) const;

  const char* getVia() const; // Get the via.
  const char* getVia(int index) const;
  
  const char* getShape() const; // Get the shape.
  const char* getShape(int index) const;
  
  int  getTaper() const; // Get the taper.
  int  getTaper(int index) const;
  
  int  getStyle() const; // Get the style.
  int  getStyle(int index) const;
  
  int  getViaRotation() const; // Get the via rotation.
  int  getViaRotation(int index) const;
  
  void getViaRect(int* deltaX1, int* deltaY1, int* deltaX2, int* deltaY2) const;
  void getViaRect(int index, int* deltaX1, int* deltaY1, int* deltaX2, int* deltaY2) const;
  
  const char* getViaRotationStr() const; // Return via rotation in string format
  const char* getViaRotationStr(int index) const;
  
  void getViaData(int* numX, int* numY, int* stepX, int* stepY) const; // 5.4.1
  void getViaData(int index, int* numX, int* numY, int* stepX, int* stepY) const;
  
  int  getWidth() const; // Get the width.
  int  getWidth(int index) const;
  
  void getPoint(int* x, int* y) const; // Get the point.
  void getPoint(int index, int* x, int* y) const;
  
  void getFlushPoint(int* x, int* y, int* ext) const; // Get the point.
  void getFlushPoint(int index, int* x, int* y, int* ext) const;
  
  void getVirtualPoint(int* x, int* y) const;
  void getVirtualPoint(int index, int* x, int* y) const;
  
  int  getMask() const;
  int  getMask(int index) const;
  
  int  getViaTopMask() const;
  int  getViaTopMask(int index) const;
  
  int  getViaCutMask() const;
  int  getViaCutMask(int index) const;
  
  int  getViaBottomMask() const;
  int  getViaBottomMask(int index) const;
  
  int  numProps() const;
  const defiProp* prop(int index) const;
  int             hasShield() const;
  const char*     shieldName() const;


  // These routines are called by the parser to fill the path.
  void addWidth(int w);
  void addPoint(int x, int y);
  void addFlushPoint(int x, int y, int ext);
  void addVirtualPoint(int x, int y);
  void addLayer(const char* layer);
  void addVia(const char* name);
  void addViaRotation(int orient);
  void addViaRect(int deltaX1, int deltaY1, int deltaX2, int deltaY2);
  void addMask(int colorMask);
  void addViaMask(int colorMask);
  void addViaData(int numX, int numY, int stepX, int stepY);   // 5.4.1
  void setTaper();
  void addTaperRule(const char* rule);
  void addShape(const char* shape);
  void addStyle(int style);
  void addProp(defiProp* prop);
  void setShield(const char* name);

  // debug printing
  void print(FILE* fout) const;

  void bumpSize(int size);

protected:
  int currentType() const;

  int* keys_;           // type of item in path
  void** data_;         // extra data
  int numUsed_;         // number of items used in array
  int numAllocated_;    // allocated size of keys and data
  int* pointer_;        // traversal pointer, allocated because used
                        // as iterator in const traversal functions.
  int numX_;      
  int numY_;
  int stepX_;
  int stepY_;
  int deltaX_;
  int deltaY_;
  int mask_;
  defrProps* props_;
  defrString* shield_;

  defrData *defData;
};

END_LEFDEF_PARSER_NAMESPACE

USE_LEFDEF_PARSER_NAMESPACE

#endif
