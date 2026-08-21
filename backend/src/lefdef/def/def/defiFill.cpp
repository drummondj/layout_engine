// *****************************************************************************
// *****************************************************************************
// Copyright 2013 - 2015, Cadence Design Systems
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
//  $Date: 2025/03/27 $
//  $State:  $
// *****************************************************************************
// *****************************************************************************

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "lex.h"
#include "defiFill.hpp"
#include "defiDebug.hpp"

BEGIN_LEFDEF_PARSER_NAMESPACE



// *****************************************************************************
// defiFillRect class constructor.
// *****************************************************************************
defiFillRect::defiFillRect(int xl, int yl, int xh, int yh)
: m_xl(xl),
  m_yl(yl),
  m_xh(xh),
  m_yh(yh)
{
}



// *****************************************************************************
// defiFillPoly class constructor, move constructor and destructor.
//
// NOTE: The move constructor is used by std::vector during reallocation.
// *****************************************************************************
defiFillPoly::defiFillPoly(int numPts, const int *x, const int*y)
{
    m_pts.numPoints = numPts;
    m_pts.x = new int[numPts];
    m_pts.y = new int[numPts];
    
    for (int i = 0; i < numPts; i++) {
        m_pts.x[i] = x[i];
        m_pts.y[i] = y[i];
    }
}

defiFillPoly::defiFillPoly(defiFillPoly&& other) noexcept
: m_pts(other.m_pts)
{
    other.m_pts.x = nullptr;
    other.m_pts.y = nullptr;
}

defiFillPoly::~defiFillPoly()
{
    delete[] m_pts.x;
    delete[] m_pts.y;

    m_pts.numPoints = 0;
    m_pts.x = nullptr;
    m_pts.y = nullptr;
}



////////////////////////////////////////////////////
////////////////////////////////////////////////////
//
//    defiFill
//
////////////////////////////////////////////////////
////////////////////////////////////////////////////

defiFill::defiFill(defrData *data) 
: defData(data),
  viaPts_(NULL),
  viaName_(NULL)
{
  Init();
}


void defiFill::Init() {
  numPts_ = 0;
  clear();
  layerNameLength_ = 0;
  layerName_ = 0;
  viaName_ = 0;
  viaNameLength_ = 0;
  viaPts_ = 0;
  ptsAllocated_ = 0;
  viaPts_ = 0;
  viaOrients_ = new defrIntsInts();
  viaProps_ = new defrProps();
  layerProps_ = new defrProps();
}

defiFill::~defiFill() {
  Destroy();
}

void defiFill::clear() {
  hasLayer_ = 0;
  layerOpc_ = 0;
  hasVia_ = 0;
  viaOpc_ = 0;
  mask_ = 0;
}

void
defiFill::clearShapes() {
    m_rects.clear();
    m_polys.clear();
}

void defiFill::clearPts() {
  struct defiPoints* p;
  int i;

  for (i = 0; i < numPts_; i++) {
    p = viaPts_[i];
    free((char*)(p->x));
    free((char*)(p->y));
    free((char*)(viaPts_[i]));
  }
  numPts_ = 0;

  delete viaProps_;
  delete viaOrients_;
  delete layerProps_;

  viaOrients_ = new defrIntsInts();
  viaProps_ = new defrProps();
  layerProps_ = new defrProps();
}

void defiFill::Destroy() {
  if (layerName_) free(layerName_);
  if (viaName_) free(viaName_);
  clearShapes();
  clearPts();
  if (viaPts_) free((char*)(viaPts_));
  viaPts_ = 0;

  delete viaProps_;
  delete viaOrients_;
  delete layerProps_;

  clear();
}


void defiFill::setLayer(const char* name) {
  int len = (int) strlen(name) + 1;
  if (layerNameLength_ < len) {
    if (layerName_) free(layerName_);
    layerName_ = (char*)malloc(len);
    layerNameLength_ = len;
  }
  strcpy(layerName_, defData->DEFCASE(name));
  hasLayer_ = 1;
}

// 5.7
void defiFill::setLayerOpc() {
  layerOpc_ = 1;
}

void defiFill::addRect(int xl, int yl, int xh, int yh) {
    m_rects.emplace_back(xl, yl, xh, yh);
}

// 5.6
void defiFill::addPolygon(defiGeometries* geom) {
    m_polys.emplace_back(geom->numPoints(), geom->x(), geom->y());
}

// 6.0
void defiFill::addLayerProp(defiProp* prop)
{
    layerProps_->push_back(prop);
}

int defiFill::hasLayer() const {
  return hasLayer_;
}

const char* defiFill::layerName() const {
  return layerName_;
}

// 5.7
int defiFill::hasLayerOpc() const {
  return layerOpc_;
}


size_t defiFill::numRectangles() const {
    return m_rects.size();
}

int defiFill::xl(size_t index) const {
    if (index >= m_rects.size()) {
        defiError(1, 0, "bad index for Fill xl", defData);
        return 0;
    }

    return m_rects[index].m_xl;
}

int defiFill::yl(size_t index) const {
    if (index >= m_rects.size()) {
        defiError(1, 0, "bad index for Fill yl", defData);
        return 0;
    }

    return m_rects[index].m_yl;
}

int defiFill::xh(size_t index) const {
    if (index >= m_rects.size()) {
        defiError(1, 0, "bad index for Fill xh", defData);
        return 0;
    }

    return m_rects[index].m_xh;
}

int defiFill::yh(size_t index) const {
    if (index >= m_rects.size()) {
        defiError(1, 0, "bad index for Fill yh", defData);
        return 0;
    }

    return m_rects[index].m_yh;
}

const defiFillRect &
defiFill::rect(size_t index) const {
    return m_rects[index];
}


// 5.6
size_t defiFill::numPolygons() const {
    return m_polys.size();
}


// 5.6
const defiPoints &
defiFill::getPolygon(size_t index) const {
    return m_polys[index].m_pts;
}

// 5.7
void defiFill::setVia(const char* name) {
  int len = (int) strlen(name) + 1;
  if (viaNameLength_ < len) {
    if (viaName_) free(viaName_);
    viaName_ = (char*)malloc(len);
    viaNameLength_ = len;
  }
  strcpy(viaName_, defData->DEFCASE(name));
  hasVia_ = 1;
}

// 5.7
void defiFill::setViaOpc() {
  viaOpc_ = 1;
}

// 6.0
void defiFill::addViaProp(defiProp* prop)
{
    viaProps_->push_back(prop);
}

// 5.8 
void defiFill::setMask(int colorMask) {
    mask_ = colorMask;
}

// 5.7
void defiFill::addPts(defiGeometries* geom, defrInts *orients) {
  struct defiPoints* p;
  int x, y;
  int i;

  if (numPts_ == ptsAllocated_) {
    struct defiPoints** pts;
    ptsAllocated_ = (ptsAllocated_ == 0) ?
          2 : ptsAllocated_ * 2;
    pts= (struct defiPoints**)malloc(sizeof(struct defiPoints*) *
            ptsAllocated_);
    for (i = 0; i < numPts_; i++)
      pts[i] = viaPts_[i];
    if (viaPts_)
      free((char*)(viaPts_));
    viaPts_ = pts;
  }
  p = (struct defiPoints*)malloc(sizeof(struct defiPoints));
  p->numPoints = geom->numPoints();
  p->x = (int*)malloc(sizeof(int)*p->numPoints);
  p->y = (int*)malloc(sizeof(int)*p->numPoints);

  viaOrients_->push_back(defrInts());

  for (i = 0; i < p->numPoints; i++) {
    geom->points(i, &x, &y);
    p->x[i] = x;
    p->y[i] = y;

    (*viaOrients_)[viaOrients_->size() - 1].push_back((*orients)[i]);
  }

  viaPts_[numPts_] = p;
  numPts_ += 1;
}

// 5.7
int defiFill::hasVia() const {
  return hasVia_;
}

// 5.7
const char* defiFill::viaName() const {
  return viaName_;
}

// 5.7
int defiFill::hasViaOpc() const {
  return viaOpc_;
}

// 5.7
int defiFill::numViaPts() const {
  return numPts_;
}

// 5.8
int defiFill::layerMask() const {
    return mask_;
}

// 5.8
int defiFill::viaTopMask() const {
    return mask_ / 100;
}

// 5.8
int defiFill::viaCutMask() const {
    return mask_ / 10 % 10;
}

// 5.8
int defiFill::viaBottomMask() const {
    return mask_ % 10;
}

int defiFill::numLayerProps() const
{
    return (int) layerProps_->size();
}

defiProp* defiFill::layerProp(int index) const
{
    return (*layerProps_)[index];
}

// 5.7
struct defiPoints defiFill::getViaPts(int index) const {
  return *(viaPts_[index]);
}

// 6.0
int defiFill::getViaOrient(int index, int viaIdx) const
{
    return (*viaOrients_)[index][viaIdx];
}

// 6.0
int defiFill::numViaProps() const
{
    return (int) viaProps_->size();
}

// 6.0
const defiProp* defiFill::viaProp(int index) const
{
    return (*viaProps_)[index];
}

void defiFill::print(FILE* f) const {
  int i, j;
  struct defiPoints points;

  if (hasLayer())
    fprintf(f, "- LAYER %s", layerName());

  if (layerMask())
      fprintf(f, " + Mask %d", layerMask());

  if (hasLayerOpc())
    fprintf(f, " + OPC");
  fprintf(f, "\n");

  for (i = 0; i < numRectangles(); i++) {
    fprintf(f, "   RECT %d %d %d %d\n", xl(i),
            yl(i), xh(i),
            yh(i));
  }

  for (i = 0; i < numPolygons(); i++) {
    fprintf(f, "   POLYGON ");
    points = getPolygon(i);
    for (j = 0; j < points.numPoints; j++)
      fprintf(f, "%d %d ", points.x[j], points.y[j]);
    fprintf(f, "\n");
  }
  fprintf(f,"\n");

  if (hasVia())
    fprintf(f, "- VIA %s", viaName());

  if (mask_) {
      fprintf(f, " + MASK %d%d%d", viaTopMask(),
          viaCutMask(),
          viaBottomMask());
  }

  if (hasViaOpc())
    fprintf(f, " + OPC");
  fprintf(f, "\n");

  for (i = 0; i < numViaPts(); i++) {
    fprintf(f, "   ");
    points = getViaPts(i);
    for (j = 0; j < points.numPoints; j++)
      fprintf(f, "%d %d ", points.x[j], points.y[j]);
    fprintf(f, "\n");
  }
  fprintf(f,"\n");
}
END_LEFDEF_PARSER_NAMESPACE

