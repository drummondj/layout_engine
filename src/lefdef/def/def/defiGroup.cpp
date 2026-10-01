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
//  $Revision: #11 $
//  $Date: 2021/08/27 $
//  $State:  $
// *****************************************************************************
// *****************************************************************************

#include <string.h>
#include <stdlib.h>
#include "lex.h"
#include "defiGroup.hpp"
#include "defiDebug.hpp"

BEGIN_LEFDEF_PARSER_NAMESPACE

//////////////////////////////////////////////
//////////////////////////////////////////////
//
//   defiGroup
//
//////////////////////////////////////////////
//////////////////////////////////////////////


defiGroup::defiGroup(defrData *data) 
: defData(data),
  groups_(NULL)
{
  Init();
}


void defiGroup::Init() {
  name_ = 0;
  nameLength_ = 0;
  region_ = 0;
  regionLength_ = 0;

  numRects_ = 0;
  rectsAllocated_ = 2;
  xl_ = (int*)malloc(sizeof(int)*2);
  yl_ = (int*)malloc(sizeof(int)*2);
  xh_ = (int*)malloc(sizeof(int)*2);
  yh_ = (int*)malloc(sizeof(int)*2);

  numProps_ = 0; 
  propsAllocated_ = 2;
  propNames_   = (char**)malloc(sizeof(char*)*2);
  propValues_  = (char**)malloc(sizeof(char*)*2);
  propDValues_ = (double*)malloc(sizeof(double)*2);
  propTypes_   = (char*)malloc(sizeof(char)*2);
  hasPowerdomain_ = 0;
  groups_ = NULL;
  hinsts_ = NULL;
  components_ = NULL;
}


defiGroup::~defiGroup() {
  Destroy();
}


void defiGroup::Destroy() {

  if (name_) free(name_);
  if (region_) free(region_);
  name_ = 0;
  nameLength_ = 0;
  region_ = 0;
  regionLength_ = 0;

  clear();
  free((char*)(propNames_));
  free((char*)(propValues_));
  free((char*)(propDValues_));
  free((char*)(propTypes_));
  free((char*)(xl_));
  free((char*)(yl_));
  free((char*)(xh_));
  free((char*)(yh_));

}


void defiGroup::clear() {
  int i;
  hasRegionName_ = 0;
  hasPerim_ = 0;
  hasMaxX_ = 0;
  hasMaxY_ = 0;
  for (i = 0; i < numProps_; i++) {
    free(propNames_[i]);
    free(propValues_[i]);
    propDValues_[i] = 0;
  }
  numProps_ = 0;
  numRects_ = 0;
  
  delete groups_;
  groups_ = NULL;

  delete hinsts_;
  hinsts_ = NULL;

  delete components_;
  components_ = NULL;

  hasPowerdomain_ = 0;
}


void defiGroup::setup(const char* name) {
  int len = (int) strlen(name) + 1;
  if (len > nameLength_) {
    if (name_) free(name_);
    nameLength_ = len;
    name_ = (char*)malloc(len);
  }
  strcpy(name_, defData->DEFCASE(name));
  clear();

}


void defiGroup::addRegionRect(int xl, int yl, int xh, int yh) {
  int i;
  if (numRects_ == rectsAllocated_) {
    int max = numRects_ * 2;
    int* nxl = (int*)malloc(sizeof(int)*max);
    int* nyl = (int*)malloc(sizeof(int)*max);
    int* nxh = (int*)malloc(sizeof(int)*max);
    int* nyh = (int*)malloc(sizeof(int)*max);
    max = numRects_;
    for (i = 0; i < max; i++) {
      nxl[i] = xl_[i];
      nyl[i] = yl_[i];
      nxh[i] = xh_[i];
      nyh[i] = yh_[i];
    }
    free((char*)(xl_));
    free((char*)(yl_));
    free((char*)(xh_));
    free((char*)(yh_));
    xl_ = nxl;
    yl_ = nyl;
    xh_ = nxh;
    yh_ = nyh;
    rectsAllocated_ *= 2;
  }

  i = numRects_;
  xl_[i] = xl;
  yl_[i] = yl;
  xh_[i] = xh;
  yh_[i] = yh;
  numRects_ += 1;
}


void defiGroup::regionRects(int* size, int** xl,
   int**yl, int** xh, int** yh) const {
  *size = numRects_;
  *xl = xl_;
  *yl = yl_;
  *xh = xh_;
  *yh = yh_;
}


void defiGroup::setRegionName(const char* region) {
  int len = (int) strlen(region) + 1;
  if (len > regionLength_) {
    if (region_) free(region_);
    regionLength_ = len;
    region_ = (char*)malloc(len);
  }
  strcpy(region_, defData->DEFCASE(region));
  hasRegionName_ = 1;

}


void defiGroup::setMaxX(int x) {
  hasMaxX_ = 1;
  maxX_ = x;
}


void defiGroup::setMaxY(int y) {
  hasMaxY_ = 1;
  maxY_ = y;
}


void defiGroup::setPerim(int p) {
  hasPerim_ = 1;
  perim_ = p;
}

void defiGroup::setPowerdomain()
{
    hasPowerdomain_ = 1;
}

void defiGroup::addGroup(const char* group)
{
    if (groups_ == NULL) {
        groups_ = new defrStrings;
    }

    groups_->push_back(group);
}

void defiGroup::addHinst(const char* hinst)
{
    if (hinsts_ == NULL) {
        hinsts_ = new defrStrings;
    }

    hinsts_->push_back(hinst);
}

void defiGroup::addComponent(const char* component)
{
    if (components_ == NULL) {
        components_ = new defrStrings;
    }

    components_->push_back(component);
}


void defiGroup::addProperty(const char* name, 
                            const char* value,
                            char        type) {
  int len;
  if (numProps_ == propsAllocated_) {
    int i;
    char**  nn;
    char**  nv;
    double* nd;
    char*   nt;

    propsAllocated_ *= 2;
    nn = (char**)malloc(sizeof(char*)*propsAllocated_);
    nv = (char**)malloc(sizeof(char*)*propsAllocated_);
    nd = (double*)malloc(sizeof(double)*propsAllocated_);
    nt = (char*)malloc(sizeof(char)*propsAllocated_);
    for (i = 0; i < numProps_; i++) {
      nn[i] = propNames_[i];
      nv[i] = propValues_[i];
      nd[i] = propDValues_[i];
      nt[i] = propTypes_[i];
    }
    free((char*)(propNames_));
    free((char*)(propValues_));
    free((char*)(propDValues_));
    free((char*)(propTypes_));
    propNames_ = nn;
    propValues_ = nv;
    propDValues_ = nd;
    propTypes_ = nt;
  }
  len = (int) strlen(name) + 1;
  propNames_[numProps_] = (char*)malloc(len);
  strcpy(propNames_[numProps_], defData->DEFCASE(name));
  len = (int) strlen(value) + 1;
  propValues_[numProps_] = (char*)malloc(len);
  strcpy(propValues_[numProps_], defData->DEFCASE(value));
  propDValues_[numProps_] = 0;
  propTypes_[numProps_] = type;
  numProps_ += 1;
}


void defiGroup::addNumProperty(const char* name, double d,
                               const char* value, char type) {
  int len;
  if (numProps_ == propsAllocated_) {
    int i;
    char**  nn;
    char**  nv;
    double* nd;
    char*   nt;

    propsAllocated_ *= 2;
    nn = (char**)malloc(sizeof(char*)*propsAllocated_);
    nv = (char**)malloc(sizeof(char*)*propsAllocated_);
    nd = (double*)malloc(sizeof(double)*propsAllocated_);
    nt = (char*)malloc(sizeof(char)*propsAllocated_);
    for (i = 0; i < numProps_; i++) {
      nn[i] = propNames_[i];
      nv[i] = propValues_[i];
      nd[i] = propDValues_[i];
      nt[i] = propTypes_[i];
    }
    free((char*)(propNames_));
    free((char*)(propValues_));
    free((char*)(propDValues_));
    free((char*)(propTypes_));
    propNames_ = nn;
    propValues_ = nv;
    propDValues_ = nd;
    propTypes_ = nt;
  }
  len = (int) strlen(name) + 1;
  propNames_[numProps_] = (char*)malloc(len);
  strcpy(propNames_[numProps_], defData->DEFCASE(name));
  len = (int) strlen(value) + 1;
  propValues_[numProps_] = (char*)malloc(len);
  strcpy(propValues_[numProps_], defData->DEFCASE(value));
  propDValues_[numProps_] = d;
  propTypes_[numProps_] = type;
  numProps_ += 1;
}


int defiGroup::numProps() const {
  return numProps_;
}


const char* defiGroup::propName(int index) const {
  char msg[160];
  if (index < 0 || index >= numProps_) {
     sprintf (msg, "ERROR (LEFPARS-6050): The index number %d given for the GROUP PROPERTY is invalid.\nValid index is from 0 to %d", index, numProps_);
     defiError(0, 6050, msg, defData);
     return 0;
  }
  return propNames_[index];
}


const char* defiGroup::propValue(int index) const {
  char msg[160];
  if (index < 0 || index >= numProps_) {
     sprintf (msg, "ERROR (LEFPARS-6050): The index number %d given for the GROUP PROPERTY is invalid.\nValid index is from 0 to %d", index, numProps_);
     defiError(0, 6050, msg, defData);
     return 0;
  }
  return propValues_[index];
}


double defiGroup::propNumber(int index) const {
  char msg[160];
  if (index < 0 || index >= numProps_) {
     sprintf (msg, "ERROR (LEFPARS-6050): The index number %d given for the GROUP PROPERTY is invalid.\nValid index is from 0 to %d", index, numProps_);
     defiError(0, 6050, msg, defData);
     return 0;
  }
  return propDValues_[index];
}


char defiGroup::propType(int index) const {
  char msg[160];
  if (index < 0 || index >= numProps_) {
     sprintf (msg, "ERROR (LEFPARS-6050): The index number %d given for the GROUP PROPERTY is invalid.\nValid index is from 0 to %d", index, numProps_);
     defiError(0, 6050, msg, defData);
     return 0;
  }
  return propTypes_[index];
}


int defiGroup::propIsNumber(int index) const {
  char msg[160];
  if (index < 0 || index >= numProps_) {
     sprintf (msg, "ERROR (LEFPARS-6050): The index number %d given for the GROUP PROPERTY is invalid.\nValid index is from 0 to %d", index, numProps_);
     defiError(0, 6050, msg, defData);
     return 0;
  }
  return propDValues_[index] ? 1 : 0;
}


int defiGroup::propIsString(int index) const {
  char msg[160];
  if (index < 0 || index >= numProps_) {
     sprintf (msg, "ERROR (LEFPARS-6050): The index number %d given for the GROUP PROPERTY is invalid.\nValid index is from 0 to %d", index, numProps_);
     defiError(0, 6050, msg, defData);
     return 0;
  }
  return propDValues_[index] ? 0 : 1;
}

int defiGroup::hasPowerdomain() const
{
    return hasPowerdomain_;
}

int defiGroup::numGroups() const
{
    if (groups_ == NULL) {
        return 0;
    }

    return (int) groups_->size();
}

int defiGroup::numHinsts() const
{
    if (hinsts_ == NULL) {
        return 0;
    }

    return (int) hinsts_->size();
}

int defiGroup::numComponents() const
{
    if (components_ == NULL) {
        return 0;
    }

    return (int) components_->size();
}

const char* defiGroup::group(int idx) const
{
    return (*groups_)[idx].c_str();
}

const char* defiGroup::hinst(int idx) const
{
    return (*hinsts_)[idx].c_str();
}

const char* defiGroup::component(int idx) const
{
    return (*components_)[idx].c_str();
}


const char* defiGroup::regionName() const {
  return region_;
}


const char* defiGroup::name() const {
  return name_;
}


int defiGroup::perim() const {
  return perim_;
}


int defiGroup::maxX() const {
  return maxX_;
}


int defiGroup::maxY() const {
  return maxY_;
}


int defiGroup::hasMaxX() const {
  return hasMaxX_;
}


int defiGroup::hasMaxY() const {
  return hasMaxY_;
}


int defiGroup::hasPerim() const {
  return hasPerim_;
}


int defiGroup::hasRegionBox() const {
  return numRects_ ? 1 : 0 ;
}


int defiGroup::hasRegionName() const {
  return hasRegionName_;
}


void defiGroup::print(FILE* f) const {
  int i;

  fprintf(f, "Group '%s'\n", name());

  if (hasRegionName()) {
    fprintf(f, "  region name '%s'\n", regionName());
  }

  if (hasRegionBox()) {
    int size = numRects_;
    int* xl = xl_;
    int* yl = yl_;
    int* xh = xh_;
    int* yh = yh_;
    for (i = 0; i < size; i++)
      fprintf(f, "  region box %d,%d %d,%d\n", xl[i], yl[i], xh[i], yh[i]);
  }

  if (hasMaxX()) {
    fprintf(f, "  max x %d\n", maxX());
  }

  if (hasMaxY()) {
    fprintf(f, "  max y %d\n", maxY());
  }

  if (hasPerim()) {
    fprintf(f, "  perim %d\n", perim());
  }

}


END_LEFDEF_PARSER_NAMESPACE

