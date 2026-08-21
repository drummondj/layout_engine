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
//  $Revision: #17 $
//  $Date: 2023/08/16 $
//  $State:  $
// *****************************************************************************
// *****************************************************************************

#include <stdlib.h>
#include <string.h>
#include "defiPath.hpp"
#include "defiDebug.hpp"
#include "lex.h"
#include "defiUtil.hpp"

BEGIN_LEFDEF_PARSER_NAMESPACE

defiPath::defiPath(defrData*  data)
: defData(data),
  keys_(NULL),
  data_(NULL),
  pointer_(NULL),
  numUsed_(0),
  numAllocated_(0),
  numX_(0),      
  numY_(0),
  stepX_(0),
  stepY_(0),
  deltaX_(0),
  deltaY_(0),
  mask_(0),
  props_(0),
  shield_(0)
{
}


defiPath::defiPath(defiPath *defiPathRef) 
{
    *this = *defiPathRef;

    defiPathRef->pointer_ = NULL;
    defiPathRef->keys_ = NULL;
    defiPathRef->data_ = NULL;    
    defiPathRef->props_ = NULL;
    defiPathRef->shield_ = NULL;
}


defiPath::~defiPath()
{
    Destroy();
}


void
defiPath::Init()
{
    // Should do nothing in constructor case. 
    Destroy();

    numUsed_ = 0;
    numAllocated_ = 0;
    pointer_ = new int;
    *pointer_ = -1;

    numX_ = 0;      
    numY_ = 0;
    stepX_ = 0;
    stepY_ = 0;
    deltaX_ = 0;
    deltaY_ = 0;
    mask_ = 0;
    props_ = 0;
    shield_ = 0;

    bumpSize(16);
}


void
defiPath::clear()
{
    for (int i = 0; i < numUsed_; i++) {
        free(data_[i]);
        data_[i] = NULL;
    }

    numUsed_ = 0;

    delete pointer_;
    pointer_ = NULL;

    delete props_;
    props_ = NULL;

    delete shield_;
    shield_ = NULL;
}


void
defiPath::Destroy()
{
    clear();

    if (keys_) {
       free(keys_);
       keys_ = NULL;
    }

    if (data_) {
       free(data_);
       data_ = NULL;
    }

}


void defiPath::reverseOrder() {
  int one = 0;
  int two = numUsed_ - 1;
  int t;
  void *tptr;
  while (one < two) {
    t = keys_[one];
    keys_[one] = keys_[two];
    keys_[two] = t;
    tptr = data_[one];
    data_[one] = data_[two];
    data_[two] = tptr;
    one++;
    two--;
  }
}


void defiPath::initTraverse() const
{
    *(pointer_) = -1;
}


void defiPath::initTraverseBackwards() const
{
    *(pointer_) = numUsed_;
}


int 
defiPath::currentType() const
{
    return getType(*pointer_);
}

int defiPath::next() const 
{
    (*(pointer_))++;

    return currentType();
}


int defiPath::prev()  const
{
    (*(pointer_))--;

    return currentType();
}



// *****************************************************************************
// defiPath::getNumElements()
//
// This function returns the number of elements in the path.
// *****************************************************************************
int
defiPath::getNumElements() const
{
    return numUsed_;
}



// *****************************************************************************
// defiPath::getType()
//
// This function returns the type of 'index'ed element in the path.
// *****************************************************************************
int
defiPath::getType(int index) const
{
    if (index >= 0 && index < numUsed_) {
        switch (keys_[index]) {
            case 'L': return DEFIPATH_LAYER;
            case 'V': return DEFIPATH_VIA;
            case 'W': return DEFIPATH_WIDTH;
            case 'P': return DEFIPATH_POINT;
            case 'F': return DEFIPATH_FLUSHPOINT;
            case 'T': return DEFIPATH_TAPER;
            case 'R': return DEFIPATH_TAPERRULE;
            case 'S': return DEFIPATH_SHAPE;
            case 'Y': return DEFIPATH_STYLE;
            case 'O': return DEFIPATH_VIAROTATION;
            case 'E': return DEFIPATH_RECT;
            case 'D': return DEFIPATH_VIADATA;
            case 'U': return DEFIPATH_VIRTUALPOINT;
            case 'M': return DEFIPATH_MASK;
            case 'C': return DEFIPATH_VIAMASK;
            default : return DEFIPATH_DONE;
        }
    }

    return DEFIPATH_DONE;
}



// *****************************************************************************
// These functions return a specific path object.
// *****************************************************************************
int
defiPath::getTaper() const
{
    return getTaper(*pointer_);
}

int
defiPath::getTaper(int index) const
{
    if (keys_[index] != 'T') {
        return 0;
    }

    return 1;
}

const char*
defiPath::getTaperRule() const
{
    return getTaperRule(*pointer_);
}

const char*
defiPath::getTaperRule(int index) const
{
    if (keys_[index] != 'R') {
        return 0;
    }

    return (char*)(data_[index]);
}

const char*
defiPath::getLayer() const
{
    return getLayer(*pointer_);
}

const char*
defiPath::getLayer(int index) const
{
    if (keys_[index] != 'L') {
        return 0;
    }

    return (char*)(data_[index]);
}

const char*
defiPath::getVia() const
{
  return getVia(*pointer_);
}

const char*
defiPath::getVia(int index) const
{
    if (keys_[index] != 'V') {
        return 0;
    }

    return (char*)(data_[index]);
}


const char*
defiPath::getShape() const
{
    return getShape(*pointer_);
}


const char*
defiPath::getShape(int index) const
{
    if (keys_[index] != 'S') {
        return 0;
    }

    return (char*)(data_[index]);
}

int
defiPath::getStyle() const
{
    return getStyle(*pointer_);
}

int
defiPath::getStyle(int index) const
{
    if (keys_[index] != 'Y') {
        return 0;
    }

    int *style = (int*)(data_[index]);

    return *style;
}

int
defiPath::getWidth() const
{
    return getWidth(*pointer_);
}

int
defiPath::getWidth(int index) const
{
    if (keys_[index] != 'W') {
        return 0;
    }

    int *wptr = (int*)(data_[index]);

    return *wptr;
}

int
defiPath::getViaRotation() const
{
    return getViaRotation(*pointer_);
}

int
defiPath::getViaRotation(int index) const
{
    if (keys_[index] != 'O') {
        return 0;
    }

    int *wptr = (int*)(data_[index]);

    return *wptr;
}

int
defiPath::getMask() const
{
    return getMask(*pointer_);
}

int
defiPath::getMask(int index) const
{
    if (keys_[index] != 'M') {
        return 0;
    }

    int *wptr = (int*)(data_[index]);

    return *wptr;
}

int
defiPath::getViaBottomMask() const
{
    return getViaBottomMask(*pointer_);
}

int
defiPath::getViaBottomMask(int index) const
{
    if (keys_[index] != 'C') {
        return 0;
    }

    int *wptr = (int*)(data_[index]);

    return (*wptr) % 10;
}

int
defiPath::getViaCutMask() const
{
    return getViaCutMask(*pointer_);
}

int
defiPath::getViaCutMask(int index) const
{
    if (keys_[index] != 'C') {
        return 0;
    }

    int *wptr = (int*)(data_[index]);

    return (*wptr) / 10 % 10;
}

int
defiPath::getViaTopMask() const
{
    return getViaTopMask(*pointer_);
}

int
defiPath::getViaTopMask(int index) const
{
    if (keys_[index] != 'C') {
        return 0;
    }

    int *wptr = (int*)(data_[index]);

    return (*wptr) / 100;
}

const char*
defiPath::getViaRotationStr() const
{
    return getViaRotationStr(*pointer_);
}

const char*
defiPath::getViaRotationStr(int index) const
{
    if (keys_[index] != 'O') {
        return 0;
    }

    int *wptr = (int*)(data_[index]);

    return defiOrientStr(*wptr);
}

void
defiPath::getViaRect(int* deltaX1,
                     int* deltaY1,
                     int* deltaX2,
                     int* deltaY2) const
{
    getViaRect(*pointer_, deltaX1, deltaY1, deltaX2, deltaY2);
}

void
defiPath::getViaRect(int  index,
                     int* deltaX1,
                     int* deltaY1,
                     int* deltaX2,
                     int* deltaY2) const
{
    if (keys_[index] != 'E') {
        return;
    }

    *deltaX1 = ((struct defiViaRect*)(data_[index]))->deltaX1;
    *deltaY1 = ((struct defiViaRect*)(data_[index]))->deltaY1;
    *deltaX2 = ((struct defiViaRect*)(data_[index]))->deltaX2;
    *deltaY2 = ((struct defiViaRect*)(data_[index]))->deltaY2;
}

void
defiPath::getViaData(int* numX,
                     int* numY,
                     int* stepX,
                     int* stepY) const
{
    getViaData(*pointer_, numX, numY, stepX, stepY);
}

void
defiPath::getViaData(int  index,
                     int* numX,
                     int* numY,
                     int* stepX,
                     int* stepY) const
{
    if (keys_[index] != 'D') {
        return;
    }

    *numX = ((struct defiViaData*)(data_[index]))->numX;
    *numY = ((struct defiViaData*)(data_[index]))->numY;
    *stepX = ((struct defiViaData*)(data_[index]))->stepX;
    *stepY = ((struct defiViaData*)(data_[index]))->stepY;
}

void
defiPath::getFlushPoint(int* x,
                        int* y,
                        int* ext) const
{
    getFlushPoint(*pointer_, x, y, ext);
}

void
defiPath::getFlushPoint(int  index,
                        int* x,
                        int* y,
                        int* ext) const
{
    if (keys_[index] != 'F') {
        return;
    }

    *x = ((struct defiPnt*)(data_[index]))->x;
    *y = ((struct defiPnt*)(data_[index]))->y;
    *ext = ((struct defiPnt*)(data_[index]))->ext;
}

void
defiPath::getVirtualPoint(int* x,
                          int* y) const
{
    getVirtualPoint(*pointer_, x, y);
}

void
defiPath::getVirtualPoint(int  index,
                          int* x,
                          int* y) const
{
    if (keys_[index] != 'U') {
        return;
    }

    *x = ((struct defiPnt*)(data_[index]))->x;
    *y = ((struct defiPnt*)(data_[index]))->y;
}

void
defiPath::getPoint(int* x,
                   int* y) const
{
    getPoint(*pointer_, x, y);
}

void
defiPath::getPoint(int  index,
                   int* x,
                   int* y) const
{
    if (keys_[index] != 'P') {
        return;
    }

    *x = ((struct defiPnt*)(data_[index]))->x;
    *y = ((struct defiPnt*)(data_[index]))->y;
}



int defiPath::numProps() const
{
    if (!props_) {
        return 0;
    }

    return (int) props_->size();
}

const defiProp* defiPath::prop(int index) const
{
    return (*props_)[index];
}

int defiPath::hasShield() const
{
    return shield_ ? 1 : 0;
}

const char* defiPath::shieldName() const
{
    if (!shield_) {
        return NULL;
    }

    return shield_->c_str();
}


void defiPath::addWidth(int w) {
  int * wValue;
  if (numUsed_ == numAllocated_)
    bumpSize(numAllocated_ * 2);
  wValue = (int*)malloc(sizeof(int));
  *wValue = w;
  keys_[numUsed_] = 'W';
  data_[numUsed_] = wValue;
  (numUsed_)++;
}


void defiPath::addVia(const char* l) {
  int len = (int) strlen(l)+1;
  if (numUsed_ == numAllocated_)
    bumpSize(numAllocated_ * 2);
  keys_[numUsed_] = 'V';
  data_[numUsed_] = malloc(len);
  strcpy((char*)(data_[numUsed_]), defData->DEFCASE(l));
  (numUsed_)++;
}


void defiPath::addViaRotation(int o) {
  int * orient;
  if (numUsed_ == numAllocated_)
    bumpSize(numAllocated_ * 2);
  orient = (int*)malloc(sizeof(int));
  *orient = o;
  keys_[numUsed_] = 'O';
  data_[numUsed_] = orient;
  (numUsed_)++;
}


void defiPath::addViaRect(int deltaX1, int deltaY1, int deltaX2, int deltaY2) {
    if (numUsed_ == numAllocated_)
        bumpSize(numAllocated_ * 2);
    keys_[numUsed_] = 'E';  // RECT
    data_[numUsed_] = malloc(sizeof(struct defiViaRect));
    ((struct defiViaRect*)(data_[numUsed_]))->deltaX1 = deltaX1;
    ((struct defiViaRect*)(data_[numUsed_]))->deltaY1 = deltaY1;
    ((struct defiViaRect*)(data_[numUsed_]))->deltaX2 = deltaX2;
    ((struct defiViaRect*)(data_[numUsed_]))->deltaY2 = deltaY2;
    (numUsed_)++;
}


void defiPath::addViaData(int numX, int numY, int stepX, int stepY) {
  if (numUsed_ == numAllocated_)
    bumpSize(numAllocated_ * 2);
  keys_[numUsed_] = 'D';
  data_[numUsed_] = malloc(sizeof(struct defiViaData));
  ((struct defiViaData*)(data_[numUsed_]))->numX = numX;
  ((struct defiViaData*)(data_[numUsed_]))->numY = numY;
  ((struct defiViaData*)(data_[numUsed_]))->stepX = stepX;
  ((struct defiViaData*)(data_[numUsed_]))->stepY = stepY;
  (numUsed_)++;
}


void defiPath::addLayer(const char* l) {
  int len = (int) strlen(l)+1;
  if (numUsed_ == numAllocated_)
    bumpSize(numAllocated_ * 2);
  keys_[numUsed_] = 'L' ;
  data_[numUsed_] = malloc(len);
  strcpy((char*)(data_[numUsed_]), defData->DEFCASE(l));
  (numUsed_)++;
}


void defiPath::addTaperRule(const char* l) {
  int len = (int) strlen(l)+1;
  if (numUsed_ == numAllocated_)
    bumpSize(numAllocated_ * 2);
  keys_[numUsed_] = 'R' ;
  data_[numUsed_] = malloc(len);
  strcpy((char*)(data_[numUsed_]), defData->DEFCASE(l));
  (numUsed_)++;
}


void defiPath::addPoint(int x, int y) {
  if (numUsed_ == numAllocated_)
    bumpSize(numAllocated_ * 2);
  keys_[numUsed_] = 'P';
  data_[numUsed_] = malloc(sizeof(struct defiPnt));
  ((struct defiPnt*)(data_[numUsed_])) -> x = x;
  ((struct defiPnt*)(data_[numUsed_])) -> y = y;
  (numUsed_)++;
}

void defiPath::addMask(int colorMask) {
    int* mask;
    if (numUsed_ == numAllocated_)
        bumpSize(numAllocated_ * 2);
    mask = (int*)malloc(sizeof(int));
    *mask = colorMask;
    keys_[numUsed_] = 'M';  //Mask for points
    data_[numUsed_] = mask;
    (numUsed_)++;
}

void defiPath::addViaMask(int colorMask) {
    int* mask;
    if (numUsed_ == numAllocated_)
        bumpSize(numAllocated_ * 2);
    mask = (int*)malloc(sizeof(int));
    *mask = colorMask;
    keys_[numUsed_] = 'C';  //viaMask
    data_[numUsed_] = mask;
    (numUsed_)++;
}

void defiPath::addFlushPoint(int x, int y, int ext) {
  if (numUsed_ == numAllocated_)
    bumpSize(numAllocated_ * 2);
  keys_[numUsed_] = 'F';
  data_[numUsed_] = malloc(sizeof(struct defiPnt));
  ((struct defiPnt*)(data_[numUsed_])) -> x = x;
  ((struct defiPnt*)(data_[numUsed_])) -> y = y;
  ((struct defiPnt*)(data_[numUsed_])) -> ext = ext;
  (numUsed_)++;
}

void defiPath::addVirtualPoint(int x, int y) {
    if (numUsed_ == numAllocated_)
        bumpSize(numAllocated_ * 2);
    keys_[numUsed_] = 'U';
    data_[numUsed_] = malloc(sizeof(struct defiPnt));
    ((struct defiPnt*)(data_[numUsed_])) -> x = x;
    ((struct defiPnt*)(data_[numUsed_])) -> y = y;
    (numUsed_)++;
}

void defiPath::setTaper() {
  if (numUsed_ == numAllocated_)
    bumpSize(numAllocated_ * 2);
  keys_[numUsed_] = 'T';
  data_[numUsed_] = 0;
  (numUsed_)++;
}


void defiPath::addShape(const char* l) {
  int len = (int) strlen(l)+1;
  if (numUsed_ == numAllocated_)
    bumpSize(numAllocated_ * 2);
  keys_[numUsed_] = 'S' ;
  data_[numUsed_] = malloc(len);
  strcpy((char*)(data_[numUsed_]), defData->DEFCASE(l));
  (numUsed_)++;
}

void defiPath::addStyle(int s) {
  int *style;
  if (numUsed_ == numAllocated_)
    bumpSize(numAllocated_ * 2);
  style = (int*)malloc(sizeof(int));
  *style = s;
  keys_[numUsed_] = 'Y' ;
  data_[numUsed_] = style;
  (numUsed_)++;
}

void defiPath::addProp(defiProp* prop)
{
    if (!props_) {
        props_ = new defrProps();
    }

    props_->push_back(prop);
}

void defiPath::setShield(const char* name)
{
    if (shield_) {
        delete shield_;
    }

    shield_ = new defrString();
    shield_->assign(name);
}


void defiPath::print(FILE* fout) const {
  int i;
  int *wptr;
  if (fout == 0) fout = stdout;
  fprintf(fout, "Path:\n");
  for (i = 0; i < numUsed_; i++) {
    if (keys_[i] == 'L') {
        fprintf(fout, " layer %s\n", (char*)(data_[i]));
    } else if (keys_[i] == 'R') {
        fprintf(fout, " taperrule %s\n", data_[i] ? (char*)(data_[i]) : "" );
    } else if (keys_[i] == 'T') {
        fprintf(fout, " taper %s\n", data_[i] ? (char*)(data_[i]) : "" );
    } else if (keys_[i] == 'S') {
        fprintf(fout, " shape %s\n", data_[i] ? (char*)(data_[i]) : "" );
    } else if (keys_[i] == 'V') {
        fprintf(fout, " via %s\n", data_[i] ? (char*)(data_[i]) : "" );
    } else if (keys_[i] == 'O') {
        fprintf(fout, " via rotation %s\n",
                data_[i] ? (char*)(data_[i]) : "" );
    } else if (keys_[i] == 'M') {
        fprintf(fout, " mask %d\n", getMask() );
    } else if (keys_[i] == 'E') {
        fprintf(fout, " rect %d,%d,%d,%d\n",
                ((struct defiViaRect*)(data_[i]))->deltaX1,
                ((struct defiViaRect*)(data_[i]))->deltaY1,
                ((struct defiViaRect*)(data_[i]))->deltaX2,
                ((struct defiViaRect*)(data_[i]))->deltaY2);
    } else if (keys_[i] == 'W') {
        wptr = (int*)(data_[i]);
        fprintf(fout, " width %d\n", *wptr);
    } else if (keys_[i] == 'P') {
        fprintf(fout, " point %d,%d\n",
                ((struct defiPnt*)(data_[i]))->x,
                ((struct defiPnt*)(data_[i]))->y);
    } else if (keys_[i] == 'F') {
        fprintf(fout, " flushpoint %d,%d,%d\n",
                ((struct defiPnt*)(data_[i]))->x,
                ((struct defiPnt*)(data_[i]))->y,
                ((struct defiPnt*)(data_[i]))->ext);
    } else if (keys_[i] == 'U') {
        fprintf(fout, " virtualpoint %d,%d\n",
            ((struct defiPnt*)(data_[i]))->x,
            ((struct defiPnt*)(data_[i]))->y);
    } else if (keys_[i] == 'D') {
      fprintf(fout, " DO %d BY %d STEP %d %d\n",
              ((struct defiViaData*)(data_[i]))->numX,
              ((struct defiViaData*)(data_[i]))->numY,
              ((struct defiViaData*)(data_[i]))->stepX,
              ((struct defiViaData*)(data_[i]))->stepY);
    } else {
      fprintf(fout, " ERROR\n");
    }
  }
}


void defiPath::bumpSize(int size) {
  int i;
  int* newKeys = (int*)malloc(size * sizeof(int*));
  void** newData = (void**)malloc(size * sizeof(void*));

  for (i = 0; i < numUsed_; i++) {
    newKeys[i] = keys_[i];
    newData[i] = data_[i];
  }

  if (keys_) free((char*)(keys_));
  if (data_) free((char*)(data_));

  keys_ = newKeys;
  data_ = newData;
  numAllocated_ = size;
}


END_LEFDEF_PARSER_NAMESPACE

