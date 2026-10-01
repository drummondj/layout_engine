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
//  $Revision: #22 $
//  $Date: 2023/07/07 $
//  $State:  $
// *****************************************************************************
// *****************************************************************************

#include <string.h>
#include <stdlib.h>
#include "defiNet.hpp"
#include "defiPath.hpp"
#include "defiDebug.hpp"
#include "lex.h"
#include "defiUtil.hpp"

BEGIN_LEFDEF_PARSER_NAMESPACE

#define	maxLimit   65536


////////////////////////////////////////////////////
////////////////////////////////////////////////////
//
//    defiSubnet
//
////////////////////////////////////////////////////
////////////////////////////////////////////////////


defiSubnet::defiSubnet(defrData *data)
 : defData(data)
{
  Init();
}


void defiSubnet::Init() {
  name_ = 0;
  bumpName(16);

  instances_ = 0;
  pins_ = 0;
  musts_ = 0;
  synthesized_ = 0;
  numPins_ = 0;
  bumpPins(16);

  // WMD -- this will be removed by the next release
  paths_ = 0;
  numPaths_ = 0;
  pathsAllocated_ = 0;

  numWires_ = 0;
  wiresAllocated_ = 0;
  wires_ = 0;
  nonDefaultRule_ = 0;

  clear();
}


void defiSubnet::Destroy() {
  clear();
  free(name_);
  free(instances_);
  free(pins_);
  free(musts_);
  free(synthesized_);

}


defiSubnet::~defiSubnet() {
  Destroy();
}


void defiSubnet::setName(const char* name) {
  int len = (int) strlen(name) + 1;
  if (len > nameSize_) bumpName(len);
  strcpy(name_, defData->DEFCASE(name));
}


void defiSubnet::setNonDefault(const char* name) {
  int len = (int) strlen(name) + 1;
  nonDefaultRule_ = (char*)malloc(len);
  strcpy(nonDefaultRule_, defData->DEFCASE(name));
}

 
void defiSubnet::addMustPin(const char* instance, const char* pin, int syn) {
  addPin(instance, pin, syn);
  musts_[numPins_ - 1] = 1;
}


void defiSubnet::addPin(const char* instance, const char* pin, int syn) {
  int len;

  if (numPins_ == pinsAllocated_)
    bumpPins(pinsAllocated_ * 2);

  len = (int) strlen(instance)+ 1;
  instances_[numPins_] = (char*)malloc(len);
  strcpy(instances_[numPins_], defData->DEFCASE(instance));

  len = (int) strlen(pin)+ 1;
  pins_[numPins_] = (char*)malloc(len);
  strcpy(pins_[numPins_], defData->DEFCASE(pin));

  musts_[numPins_] = 0;
  synthesized_[numPins_] = syn;

  (numPins_)++;
}

// WMD -- this will be removed by the next release
void defiSubnet::setType(const char* typ) {
  if (*typ == 'F') {
    isFixed_ = 1;
  } else if (*typ == 'C') {
    isCover_ = 1;
  } else if (*typ == 'R') {
    isRouted_ = 1;
  } else {
    // Silently do nothing with bad input.
  }
 
}
 
// WMD -- this will be removed by the next release
void defiSubnet::addPath(defiPath* p, int reset, int netOsnet, int *needCbk) {
  int i;
  size_t incNumber;

  if (reset) {
     for (i = 0; i < numPaths_; i++) {
        delete paths_[i];
     }  
     numPaths_ = 0;
  } 

  if (numPaths_ >= pathsAllocated_) {
    // 6/17/2003 - don't want to allocate too large memory just in case
    // a net has many wires with only 1 or 2 paths
    if (pathsAllocated_ <= maxLimit) {
        incNumber = pathsAllocated_*2;
        if (incNumber > maxLimit) {
            incNumber = pathsAllocated_ + maxLimit;
        }
    } else {
        incNumber = pathsAllocated_ + maxLimit;
    }

    switch (netOsnet) {
      case 2: 
         bumpPaths(
            pathsAllocated_ ? incNumber : 1000);
         break;
      default:
         bumpPaths(
            pathsAllocated_ ? incNumber : 8);
         break;
     }
  }

  paths_[numPaths_++] = p;

  if (numPaths_ == pathsAllocated_)
    *needCbk = 1;   // pre-warn the parser it needs to realloc next time
}


void
defiSubnet::addWire(defiWire  *wire)
{
    if (numWires_ == wiresAllocated_) {
        wiresAllocated_ = wiresAllocated_ ? wiresAllocated_ * 2
                                          : 2 ;

        defiWire** newWires;

        newWires = (defiWire**)malloc(sizeof(void*) * wiresAllocated_);

        for (int i = 0; i < numWires_; i++) {
            newWires[i] = wires_[i];
        }

        if (wires_) {
            free(wires_);
        }

        wires_ = newWires;
    }

    wires_[numWires_] = wire;
    numWires_ += 1;
}

const char* defiSubnet::name() const {
  return name_;
}


int defiSubnet::hasNonDefaultRule() const {
  return nonDefaultRule_ ? 1 : 0;
}


const char* defiSubnet::nonDefaultRule() const {
  return nonDefaultRule_;
}


int defiSubnet::numConnections() const {
  return numPins_;
}


const char* defiSubnet::instance(int index) const {
  if (index >= 0 && index < numPins_)
    return instances_[index];
  return 0;
}


const char* defiSubnet::pin(int index) const {
  if (index >= 0 && index < numPins_)
    return pins_[index];
  return 0;
}


int defiSubnet::pinIsMustJoin(int index) const {
  if (index >= 0 && index < numPins_)
    return (int)(musts_[index]);
  return 0;
}


int defiSubnet::pinIsSynthesized(int index) const {
  if (index >= 0 && index < numPins_)
    return (int)(synthesized_[index]);
  return 0;
}

// WMD -- this will be removed by the next release
int defiSubnet::isFixed() const {
  return (int)(isFixed_);
}
 
 
// WMD -- this will be removed by the next release
int defiSubnet::isRouted() const {
  return (int)(isRouted_);
}
 
 
// WMD -- this will be removed by the next release
int defiSubnet::isCover() const {
  return (int)(isCover_);
}


void defiSubnet::bumpName(long long  size) {
  if (name_) free(name_);
  name_ = (char*)malloc(size);
  nameSize_ = size;
  name_[0] = '\0';
}


void defiSubnet::bumpPins(long long size) {
  char** newInstances = (char**)malloc(sizeof(char*)*size);
  char** newPins = (char**)malloc(sizeof(char*)*size);
  char* newMusts = (char*)malloc(size);
  char* newSyn = (char*)malloc(size);
  long long i;

  if (instances_) {
    for (i = 0; i < pinsAllocated_; i++) {
      newInstances[i] = instances_[i];
      newPins[i] = pins_[i];
      newMusts[i] = musts_[i];
      newSyn[i] = synthesized_[i];
    }
    free(instances_);
    free(pins_);
    free(musts_);
    free(synthesized_);
  }

  instances_ = newInstances;
  pins_ = newPins;
  musts_ = newMusts;
  synthesized_ = newSyn;
  pinsAllocated_ = size;
}


void defiSubnet::clear() {
  int i;

  // WMD -- this will be removed by the next release
  isFixed_ = 0;
  isRouted_ = 0;
  isCover_ = 0;
  name_[0] = '\0';

  for (i = 0; i < numPins_; i++) {
    free(instances_[i]);
    free(pins_[i]);
    instances_[i] = 0;
    pins_[i] = 0;
    musts_[i] = 0;
    synthesized_[i] = 0;
  }
  numPins_ = 0;

  // WMD -- this will be removed by the next release
  if (paths_) {
    for (i = 0; i < numPaths_; i++) {
      delete paths_[i];
    }
    delete [] paths_;
    paths_ = 0;
    numPaths_ = 0;
    pathsAllocated_ = 0;
  }

  if (nonDefaultRule_) {
    free(nonDefaultRule_);
    nonDefaultRule_ = 0;
  }

  if (numWires_) {
    for (i = 0; i < numWires_; i++) {
      delete wires_[i];
      wires_[i] = 0;
    }
    free(wires_);
    wires_ = 0;
    numWires_ = 0;
    wiresAllocated_ = 0;
  }
}


void defiSubnet::print(FILE* f) const {
  int i, j;
  const defiPath* p;
  const defiWire* w;

  fprintf(f, " subnet '%s'", name_);
  fprintf(f, "\n");

  if (hasNonDefaultRule())
    fprintf(f, "  nondefault rule %s\n",
    nonDefaultRule());

  if (numConnections()) {
    fprintf(f, "  Pins:\n");
    for (i = 0; i < numConnections(); i++) {
    fprintf(f, "   '%s' '%s'%s%s\n", 
      instance(i),
      pin(i),
      pinIsMustJoin(i) ? " MUSTJOIN" : "",
      pinIsSynthesized(i) ? " SYNTHESIZED" : "");
    }
  }

  if (numWires()) {
    fprintf(f, "  Paths:\n");
    for (i = 0; i < numWires(); i++) {
      w = wire(i);
      for (j = 0; j < w->numPaths(); j++) {
         p = w->path(j);
         p->print(f);
      }
    }
  }
}

int defiSubnet::numWires() const {
  return numWires_;
}
 
 
defiWire* defiSubnet::wire(int index) {
  if (index >= 0 && index < numWires_)
    return wires_[index];
  return 0;
}


const defiWire* defiSubnet::wire(int index) const {
    if (index >= 0 && index < numWires_)
        return wires_[index];
    return 0;
}


// WMD -- this will be removed after the next release
defiPath* defiSubnet::path(int index) {
  if (index >= 0 && index < numPaths_)
    return paths_[index];
  return 0;
}

// WMD -- this will be removed after the next release
const defiPath* defiSubnet::path(int index) const {
    if (index >= 0 && index < numPaths_)
        return paths_[index];
    return 0;
}
 
// WMD -- this will be removed after the next release
int defiSubnet::numPaths() const {
  return numPaths_;
}
 
// WMD -- this will be removed after the next release
void defiSubnet::bumpPaths(long long size) {
  long long i;
  defiPath** newPaths = new defiPath*[size];
 
  for (i = 0; i < numPaths_; i++)
    newPaths[i] = paths_[i];
 
  pathsAllocated_ = size;

  delete [] paths_;
  paths_ = newPaths;
}


////////////////////////////////////////////////////
////////////////////////////////////////////////////
//
//    defiVpin
//
////////////////////////////////////////////////////
////////////////////////////////////////////////////


defiVpin::defiVpin(defrData *data)
 : defData(data),
   yl_(0),
   yLoc_(0),
   xl_(0),
   xLoc_(0),
   yh_(0),
   xh_(0),
   status_(0),
   orient_(0),
   name_(NULL),
   layer_(NULL)
{
}


void defiVpin::Init(const char* name) {
  int len = (int) strlen(name) + 1;
  name_ = (char*)malloc(len);
  strcpy(name_, defData->DEFCASE(name));
  orient_ = -1;
  status_ = ' ';
  layer_ = 0;
}


defiVpin::~defiVpin() {
    Destroy();
}


void defiVpin::Destroy() {
  free(name_);
  if (layer_) free(layer_);
}


void defiVpin::setBounds(int xl, int yl, int xh, int yh) {
  xl_ = xl;
  yl_ = yl;
  xh_ = xh;
  yh_ = yh;
}


void defiVpin::setLayer(const char* lay) {
  int len = (int) strlen(lay)+1;
  layer_ = (char*)malloc(len);
  strcpy(layer_, lay);
}


void defiVpin::setOrient(int orient) {
  orient_ = orient;
}


void defiVpin::setLoc(int x, int y) {
  xLoc_ = x;
  yLoc_ = y;
}


void defiVpin::setStatus(char st) {
  status_ = st;
}


int defiVpin::xl() const  {
  return xl_;
}


int defiVpin::yl() const  {
  return yl_;
}


int defiVpin::xh() const  {
  return xh_;
}


int defiVpin::yh() const  {
  return yh_;
}


char defiVpin::status() const {
  return status_;
}


int defiVpin::orient() const  {
  return orient_;
}


const char* defiVpin::orientStr() const  {
  return (defiOrientStr(orient_));
}


int defiVpin::xLoc() const {
  return xLoc_;
}


int defiVpin::yLoc() const {
  return yLoc_;
}


const char* defiVpin::name() const {
  return name_;
}


const char* defiVpin::layer() const {
  return layer_;
}



////////////////////////////////////////////////////
////////////////////////////////////////////////////
//
//    defiShield
//
////////////////////////////////////////////////////
////////////////////////////////////////////////////


defiShield::defiShield(defrData *data)
 : defData(data),
   paths_(NULL),
   pathsAllocated_(0),
   numPaths_(0),
   name_(NULL)
{
}


void defiShield::Init(const char* name) {
  int len = (int) strlen(name) + 1;
  name_ = (char*)malloc(len);
  strcpy(name_, defData->DEFCASE(name));
  numPaths_ = 0;
  pathsAllocated_ = 0;
  paths_ = NULL;
}


void defiShield::Destroy() {
  clear();
}


defiShield::~defiShield() {
  Destroy();
}


void defiShield::addPath(defiPath* p, int reset, int netOsnet, int *needCbk) {
  int i;
  size_t incNumber;

  if (reset) {
     for (i = 0; i < numPaths_; i++) {
        delete paths_[i];
     }
     numPaths_ = 0;
  }
  if (numPaths_ >= pathsAllocated_) {
    // 6/17/2003 - don't want to allocate too large memory just in case
    // a net has many wires with only 1 or 2 paths

    if (pathsAllocated_ <= maxLimit) {
        incNumber = pathsAllocated_*2;
        if (incNumber > maxLimit) {
            incNumber = pathsAllocated_ + maxLimit;
        }
    } else {
        incNumber = pathsAllocated_ + maxLimit;
    }

    switch (netOsnet) {
      case 2:
        bumpPaths(
            pathsAllocated_ ? incNumber : 1000);
        break;
      default:
        bumpPaths(
            pathsAllocated_ ? incNumber : 8);
        break;
    }
  }

  paths_[numPaths_++] = p;

  if (numPaths_ == pathsAllocated_)
    *needCbk = 1;   // pre-warn the parser it needs to realloc next time
}


void defiShield::clear() {
  int       i;

  if (name_) {
    free(name_);
    name_ = 0;
  }

  if (paths_) {
    for (i = 0; i < numPaths_; i++) {
      delete paths_[i];
    }

    delete [] paths_;

    paths_ = 0;
    numPaths_ = 0;
    pathsAllocated_ = 0;
  }
}


void defiShield::bumpPaths(long long size) {
  long long i;

  defiPath** newPaths = new defiPath*[size];

  for (i = 0; i < numPaths_; i++)
    newPaths[i] = paths_[i];

  pathsAllocated_ = size;

  delete [] paths_;
  
  paths_ = newPaths;
}


int defiShield::numPaths() const {
  return numPaths_;
}


const char* defiShield::shieldName() const {
  return name_;
}

defiPath* defiShield::path(int index) {
  if (index >= 0 && index < numPaths_)
    return paths_[index];
  return 0;
}

const defiPath* defiShield::path(int index) const {
    if (index >= 0 && index < numPaths_)
        return paths_[index];
    return 0;
}


////////////////////////////////////////////////////
////////////////////////////////////////////////////
//
//    defiWire
//
////////////////////////////////////////////////////
////////////////////////////////////////////////////


defiWire::defiWire(defrData *data)
 : defData(data),
   wireShieldName_(NULL),
   type_(NULL),
   paths_(NULL),
   pathsAllocated_(0),
   numPaths_(0)
{
}


void defiWire::Init(const char* type, const char* wireShieldName) {
  int len = (int) strlen(type) + 1;
  type_ = (char*)malloc(len);
  strcpy(type_, defData->DEFCASE(type));
  if (wireShieldName) {
    wireShieldName_ = (char*)malloc(strlen(wireShieldName)+1);
    strcpy(wireShieldName_, wireShieldName);
  } else
    wireShieldName_ = 0; 
  numPaths_ = 0;
  pathsAllocated_ = 0;
  paths_ = 0;
}


void defiWire::Destroy() {
  clear();
}


defiWire::~defiWire() {
  Destroy();
}


void defiWire::addPath(defiPath* p, int reset, int netOsnet, int *needCbk) {
  int i;
  size_t incNumber;

  if (reset) {
     for (i = 0; i < numPaths_; i++) {
        delete paths_[i];
     }
     numPaths_ = 0;
  }
  if (numPaths_ >= pathsAllocated_) {
    // 6/17/2003 - don't want to allocate too large memory just in case
    // a net has many wires with only 1 or 2 paths

    if (pathsAllocated_ <= maxLimit) {
        incNumber = pathsAllocated_*2;
        if (incNumber > maxLimit) {
            incNumber = pathsAllocated_ + maxLimit;
        }
    } else {
        incNumber = pathsAllocated_ + maxLimit;
    }

    switch (netOsnet) {
      case 2:
        bumpPaths(
          pathsAllocated_  ? incNumber : 1000);
        break;
      default:
        bumpPaths(
          pathsAllocated_ ? incNumber : 8);
        break;
    }
  }

  paths_[numPaths_++] = p;

  if (numPaths_ == pathsAllocated_)
    *needCbk = 1;   // pre-warn the parser it needs to realloc next time
}


void defiWire::clear() {
  int       i;

  if (type_) {
    free(type_);
    type_ = 0;
  }

  if (wireShieldName_) {
      free(wireShieldName_);
      wireShieldName_ = 0;
  }

  if (paths_) {
    for (i = 0; i < numPaths_; i++) {
      delete paths_[i];
    }

    delete [] paths_;
    paths_ = 0;
    numPaths_ = 0;
    pathsAllocated_ = 0;
  }
}


void defiWire::bumpPaths(long long size) {
  long long i;
  defiPath** newPaths =  new defiPath*[size]; 

  for (i = 0; i < numPaths_; i++)
    newPaths[i] = paths_[i];

  pathsAllocated_ = size;
  delete [] paths_;
  paths_ = newPaths;
}


int defiWire::numPaths() const {
  return numPaths_;
}


int defiWire::hasShield() const {
    return wireShieldName_ != NULL;
}


const char* defiWire::wireType() const {
  return type_;
}

const char* defiWire::wireShieldNetName() const {
  return wireShieldName_;
}

defiPath* defiWire::path(int index) {
  if (index >= 0 && index < numPaths_)
    return paths_[index];
  return 0;
}


const defiPath* defiWire::path(int index) const {
    if (index >= 0 && index < numPaths_)
        return paths_[index];
    return 0;
}



// *****************************************************************************
// Constructor and destructor for the defiNetPoly class.
// *****************************************************************************
defiNetPoly::defiNetPoly(char           *name,
                         defiGeometries *geom,
                         int            mask,
                         char           *routeStatus,
                         char           *routeStatusShieldName,
                         char           *shapeType,
                         defrProps      *props)
{
    m_name = STRDUP(name);
    m_mask = mask;
    m_routeStatus = STRDUP(routeStatus);
    m_routeStatusShieldName = STRDUP(routeStatusShieldName);
    m_shapeType = STRDUP(shapeType);
    m_props = props;

    m_points.numPoints = geom->numPoints();
    m_points.x = (int*)malloc(sizeof(int) * m_points.numPoints);
    m_points.y = (int*)malloc(sizeof(int) * m_points.numPoints);
    memcpy(m_points.x, geom->x(), m_points.numPoints * sizeof(int));
    memcpy(m_points.y, geom->y(), m_points.numPoints * sizeof(int));
}

defiNetPoly::~defiNetPoly()
{
    free(m_name);
    free(m_routeStatus);
    free(m_routeStatusShieldName);
    free(m_shapeType);
    delete m_props;

    free(m_points.x);
    free(m_points.y);
}



// *****************************************************************************
// Constructor and destructor for the defiNetRect class.
// *****************************************************************************
defiNetRect::defiNetRect(char       *name,
                         int        xl,
                         int        yl,
                         int        xh,
                         int        yh,
                         int        mask,
                         char       *routeStatus,
                         char       *routeStatusShieldName,
                         char       *shapeType,
                         defrProps  *props)
{
    m_name = STRDUP(name);
    m_xl = xl;
    m_yl = yl;
    m_xh = xh;
    m_yh = yh;
    m_mask = mask;
    m_routeStatus = STRDUP(routeStatus);
    m_routeStatusShieldName = STRDUP(routeStatusShieldName);
    m_shapeType = STRDUP(shapeType);
    m_props = props;
}

defiNetRect::~defiNetRect()
{
    free(m_name);
    free(m_routeStatus);
    free(m_routeStatusShieldName);
    free(m_shapeType);
    delete m_props;
}



// *****************************************************************************
// Constructor and destructor for the defiNetVia class.
// *****************************************************************************
defiNetVia::defiNetVia(char           *name,
                       int            orient,
                       defiGeometries *geom,
                       int            mask,
                       char           *routeStatus,
                       char           *routeStatusShieldName,
                       char           *shapeType,
                       defrProps      *props)
{
    m_name = STRDUP(name);
    m_orient = orient;
    m_mask = mask;
    m_routeStatus = STRDUP(routeStatus);
    m_routeStatusShieldName = STRDUP(routeStatusShieldName);
    m_shapeType = STRDUP(shapeType);
    m_props = props;

    m_points.numPoints = geom->numPoints();
    m_points.x = (int*)malloc(sizeof(int) * m_points.numPoints);
    m_points.y = (int*)malloc(sizeof(int) * m_points.numPoints);
    memcpy(m_points.x, geom->x(), m_points.numPoints * sizeof(int));
    memcpy(m_points.y, geom->y(), m_points.numPoints * sizeof(int));
}

defiNetVia::~defiNetVia()
{
    free(m_name);
    free(m_routeStatus);
    free(m_routeStatusShieldName);
    free(m_shapeType);
    delete m_props;

    free(m_points.x);
    free(m_points.y);
}



////////////////////////////////////////////////////
////////////////////////////////////////////////////
//
//    defiNet
//
////////////////////////////////////////////////////
////////////////////////////////////////////////////


defiNet::defiNet(defrData *data)
: defData(data),
  numPolys_(0),
  polysAllocated_(0),
  polys_(NULL),
  numRects_(0),
  rectsAllocated_(0),
  rects_(NULL),
  numVias_(0),
  viasAllocated_(0),
  vias_(NULL)
{
    Init();
}


void defiNet::Init() {
  name_ = 0;
  instances_ = 0;
  numPins_ = 0;
  numProps_ = 0;
  propNames_ = 0;
  subnets_ = 0;
  source_ = 0;
  pattern_ = 0;
  style_ = 0;
  shieldNet_ = 0;
  original_ = 0;
  use_ = 0;
  nonDefaultRule_ = 0;
  numWires_ = 0;
  wiresAllocated_ = 0;
  wires_= 0;

  numWidths_ = 0;
  widthsAllocated_ = 0;
  wlayers_ = 0;
  wdist_ = 0;

  numSpacing_ = 0;
  spacingAllocated_ = 0;
  slayers_ = 0;
  sdist_ = 0;
  sleft_ = 0;
  sright_ = 0;

  vpins_ = 0;
  numVpins_ = 0;
  vpinsAllocated_ = 0;

  shields_ = 0;
  numShields_ = 0;
  numNoShields_ = 0;
  shieldsAllocated_ = 0;
  numShieldNet_ = 0;
  shieldNetsAllocated_ = 0;

  bumpProps(2);
  bumpName(16);
  bumpPins(16);
  bumpSubnets(2);

  numSubnets_ = 0;
  paths_ = 0;
  numPaths_ = 0;

  clear();
}

void defiNet::Destroy() {
  clear();
  free(name_);
  free(instances_);
  free(pins_);
  free(musts_);
  free(synthesized_);
  free(propNames_);
  free(propValues_);
  free(propDValues_);
  free(propTypes_);
  free(subnets_);
  if (source_) free(source_);
  if (pattern_) free(pattern_);
  if (shieldNet_) free(shieldNet_);
  if (original_) free(original_);
  if (use_) free(use_);
  if (nonDefaultRule_) free(nonDefaultRule_);
  if (wlayers_) free(wlayers_);
  if (slayers_) free(slayers_);
  if (sdist_) free(sdist_);
  if (wdist_) free(wdist_);
  if (sleft_) free(sleft_);
  if (sright_) free(sright_);
}


defiNet::~defiNet() {
  Destroy();
}


void defiNet::setName(const char* name) {
  int len = (int) strlen(name) + 1;

  if (len > nameSize_) bumpName(len);
  strcpy(name_, defData->DEFCASE(name));
}


void defiNet::addMustPin(const char* instance, const char* pin, int syn) {
  addPin(instance, pin, syn);
  musts_[numPins_ - 1] = 1;
}


void defiNet::addPin(const char* instance, const char* pin, int syn) {
  int len;

  if (numPins_ == pinsAllocated_)
    bumpPins(pinsAllocated_ * 2);

  len = (int) strlen(instance)+ 1;
  instances_[numPins_] = (char*)malloc(len);
  strcpy(instances_[numPins_], defData->DEFCASE(instance));

  len = (int) strlen(pin)+ 1;
  pins_[numPins_] = (char*)malloc(len);
  strcpy(pins_[numPins_], defData->DEFCASE(pin));

  musts_[numPins_] = 0;
  synthesized_[numPins_] = syn;

  (numPins_)++;
}


void defiNet::setWeight(int w) {
  hasWeight_ = 1;
  weight_ = w;
}


void defiNet::addProp(const char* name, const char* value, char type) {
  int len;

  if (numProps_ == propsAllocated_)
    bumpProps(propsAllocated_ * 2);

  len = (int) strlen(name)+ 1;
  propNames_[numProps_] = (char*)malloc(len);
  strcpy(propNames_[numProps_], defData->DEFCASE(name));

  len = (int) strlen(value)+ 1;
  propValues_[numProps_] = (char*)malloc(len);
  strcpy(propValues_[numProps_], defData->DEFCASE(value));

  propDValues_[numProps_] = 0;
  propTypes_[numProps_] = type;

  (numProps_)++;
}


void defiNet::addNumProp(const char* name, double d,
                         const char* value, char type) {
  int len;

  if (numProps_ == propsAllocated_)
    bumpProps(propsAllocated_ * 2);

  len = (int) strlen(name)+ 1;
  propNames_[numProps_] = (char*)malloc(len);
  strcpy(propNames_[numProps_], defData->DEFCASE(name));

  len = (int) strlen(value)+ 1;
  propValues_[numProps_] = (char*)malloc(len);
  strcpy(propValues_[numProps_], defData->DEFCASE(value));

  propDValues_[numProps_] = d;
  propTypes_[numProps_] = type;

  (numProps_)++;
}


void defiNet::addSubnet(defiSubnet* subnet) {

  if (numSubnets_ >= subnetsAllocated_)
    bumpSubnets(subnetsAllocated_ * 2);

  subnets_[numSubnets_++] = subnet;
}

// WMD -- will be removed after the next release
void defiNet::setType(const char* typ) {
  if (*typ == 'F') {
    isFixed_ = 1;
  } else if (*typ == 'C') {
    isCover_ = 1;
  } else if (*typ == 'R') {
    isRouted_ = 1;
  } else {
    // Silently do nothing with bad input.
  }
}

void
defiNet::addWire(defiWire *wire)
{
    if (numWires_ == wiresAllocated_) {
        wiresAllocated_ = wiresAllocated_ ? wiresAllocated_ * 2
                                          : 2 ;

        defiWire** newWires;

        newWires = (defiWire**)malloc(sizeof(void*) * wiresAllocated_);

        for (int i = 0; i < numWires_; i++) {
            newWires[i] = wires_[i];
        }

        if (wires_) {
            free(wires_);
        }

        wires_ = newWires;
    }

    wires_[numWires_] = wire;
    numWires_ += 1;
}



// *****************************************************************************
// This function adds 'shield' to the 'shields_' array.
//
// NOTE: 'shields_' array stores either shield or noShield information at a
//       time. When storing shield, the array is indexed using 'numShields_'.
//       When storing noShield, the array is indexed using 'numNoShields_'.
//       At any given time, only one of the two indexes would be non-zero.
// *****************************************************************************
void
defiNet::addShield(defiShield *shield,
                   bool       isNoShield)
{
    int &num = isNoShield ? numNoShields_ : numShields_;

    if (num == shieldsAllocated_) {
        shieldsAllocated_ = shieldsAllocated_ ? shieldsAllocated_ * 2
                                              : 2 ;

        defiShield** newShields;

        newShields = (defiShield**)malloc(sizeof(void*) * shieldsAllocated_);

        for (int i = 0; i < num; i++) {
            newShields[i] = shields_[i];
        }

        if (shields_) {
            free(shields_);
        }

        shields_ = newShields;
    }

    shields_[num] = shield;
    num += 1;
}



void defiNet::addShieldNet(const char* name) {
  int len;

  if (numShieldNet_ == shieldNetsAllocated_) {
     if (shieldNetsAllocated_ == 0)
        bumpShieldNets(2);
     else
        bumpShieldNets(shieldNetsAllocated_ * 2);

  }
 
  len = (int) strlen(name) + 1;
  shieldNet_[numShieldNet_] = (char*)malloc(len);
  strcpy(shieldNet_[numShieldNet_], defData->DEFCASE(name));
  (numShieldNet_)++;
}


void defiNet::changeNetName(const char* name) {
  int len = (int) strlen(name) + 1;
  if (len > nameSize_) bumpName(len);
  strcpy(name_, defData->DEFCASE(name));
}

void defiNet::changeInstance(const char* instance, int index) {
  int len;
  char errMsg[128];

  if ((index < 0) || (index > numPins_)) {
     sprintf (errMsg, "ERROR (DEFPARS-6083): The index number %d specified for the NET INSTANCE is invalid.\nValid index is from 0 to %d. Specify a valid index number and then try again.",
             index, numPins_);
     defiError(0, 6083, errMsg, defData);
  }

  len = (int) strlen(instance)+ 1;
  if (instances_[index])
    free(instances_[index]);
  instances_[index] = (char*)malloc(len);
  strcpy(instances_[index], defData->DEFCASE(instance));
  return;
}

void defiNet::changePin(const char* pin, int index) {
  int len;
  char errMsg[128];

  if ((index < 0) || (index > numPins_)) {
     sprintf (errMsg, "ERROR (DEFPARS-6084): The index number %d specified for the NET PIN is invalid.\nValid index is from 0 to %d. Specify a valid index number and then try again.",
             index, numPins_);
     defiError(0, 6084, errMsg, defData);
  }

  len = (int) strlen(pin)+ 1;
  if (pins_[index])
    free(pins_[index]);
  pins_[index] = (char*)malloc(len);
  strcpy(pins_[index], defData->DEFCASE(pin));
  return;
}

const char* defiNet::name() const {
  return name_;
}


int defiNet::weight() const {
  return weight_;
}


int defiNet::numProps() const {
  return numProps_;
}


int defiNet::hasProps() const {
  return numProps_ ? 1 : 0 ;
}


int defiNet::hasWeight() const {
  return (int)(hasWeight_);
}


const char* defiNet::propName(int index) const {
  if (index >= 0 &&  index < numProps_)
    return propNames_[index];
  return 0;
}


const char* defiNet::propValue(int index) const {
  if (index >= 0 &&  index < numProps_)
    return propValues_[index];
  return 0;
}


double defiNet::propNumber(int index) const {
  if (index >= 0 &&  index < numProps_)
    return propDValues_[index];
  return 0;
}


char defiNet::propType(int index) const {
  if (index >= 0 &&  index < numProps_)
    return propTypes_[index];
  return 0;
}


int defiNet::propIsNumber(int index) const {
  if (index >= 0 &&  index < numProps_)
    return propDValues_[index] ? 1 : 0;
  return 0; 
}


int defiNet::propIsString(int index) const {
  if (index >= 0 &&  index < numProps_)
    return propDValues_[index] ? 0 : 1;
  return 0; 
}


int defiNet::numConnections() const {
  return numPins_;
}


int defiNet::numShieldNets() const {
  return numShieldNet_;
}


const char* defiNet::instance(int index) const {
  if (index >= 0 &&  index < numPins_)
    return instances_[index];
  return 0;
}


const char* defiNet::pin(int index) const {
  if (index >= 0 &&  index < numPins_)
    return pins_[index];
  return 0;
}


int defiNet::pinIsMustJoin(int index) const {
  if (index >= 0 &&  index < numPins_)
    return (int)(musts_[index]);
  return 0;
}


int defiNet::pinIsSynthesized(int index) const {
  if (index >= 0 &&  index < numPins_)
    return (int)(synthesized_[index]);
  return 0;
}


int defiNet::hasSubnets() const {
  return numSubnets_ ? 1 : 0 ;
}


int defiNet::numSubnets() const {
  return numSubnets_;
}


defiSubnet* defiNet::subnet(int index) {
  if (index >= 0 &&  index < numSubnets_)
    return subnets_[index];
  return 0;
}


const defiSubnet* defiNet::subnet(int index) const {
    if (index >= 0 &&  index < numSubnets_)
        return subnets_[index];
    return 0;
}


int defiNet::isFixed() const {
  return (int)(isFixed_);
}
 
 
int defiNet::isRouted() const {
  return (int)(isRouted_);
}
 
 
int defiNet::isCover() const {
  return (int)(isCover_);
}
 

// this method will only call if the callback defrSNetWireCbk is set
// which will callback every wire.  Therefore, only one wire should be here
void defiNet::freeWire() {
  int i;

  if (numWires_) {
    for (i = 0; i < numWires_; i++) {
      wires_[i]->Destroy();
      delete wires_[i];
      wires_[i] = 0;
    }
    free(wires_);
    wires_ = 0;
    numWires_ = 0;
    wiresAllocated_ = 0;
  }

  clearRectPoly();
  clearVia();
}


void defiNet::freeShield() {
  int i;

  if (numShields_) {
    for (i = 0; i < numShields_; i++) {
      shields_[i]->Destroy();
      free(shields_[i]);
      shields_[i] = 0;
    }
    numShields_ = 0;
    shieldsAllocated_ = 0;
  }
}


void defiNet::print(FILE* f) const {
  int i, j, x, y, newLayer;
  int numX, numY, stepX, stepY;
  const defiPath* p;
  const defiSubnet* s;
  const defiVpin* vp;
  const defiWire* w;
  int path;

  fprintf(f, "Net '%s'", name_);
  fprintf(f, "\n");

  if (hasWeight())
    fprintf(f, "  weight=%d\n", weight());

  if (hasFixedbump())
    fprintf(f, "  fixedbump\n");

  if (hasFrequency())
    fprintf(f, "  frequency=%f\n", frequency());

  if (hasCap())
    fprintf(f, "  cap=%f\n", cap());

  if (hasSource())
    fprintf(f, "  source='%s'\n", source());

  if (hasPattern())
    fprintf(f, "  pattern='%s'\n", pattern());

  if (hasOriginal())
    fprintf(f, "  original='%s'\n", original());

  if (hasUse())
    fprintf(f, "  use='%s'\n", use());

  if (hasNonDefaultRule())
    fprintf(f, "  nonDefaultRule='%s'\n", nonDefaultRule());

  if (hasXTalk())
    fprintf(f, "  xtalk=%d\n", XTalk());

  if (hasStyle())
    fprintf(f, "  style='%d'\n", style());

  if (hasProps()) {
    fprintf(f, " Props:\n");
    for (i = 0; i < numProps(); i++) {
      fprintf(f, "  '%s' '%s'\n", propName(i),
      propValue(i));
    }
  }

  if (numConnections()) {
    fprintf(f, " Pins:\n");
    for (i = 0; i < numConnections(); i++) {
    fprintf(f, "  '%s' '%s'%s%s\n", 
      instance(i),
      pin(i),
      pinIsMustJoin(i) ? " MUSTJOIN" : "",
      pinIsSynthesized(i) ? " SYNTHESIZED" : "");
    }
  }
 
  for (i = 0; i < numVpins_; i++) {
    vp = vpin(i);
    fprintf(f,
    "  VPIN %s status '%c' layer %s %d,%d orient %s bounds %d,%d to %d,%d\n",
    vp->name(),
    vp->status(),
    vp->layer() ? vp->layer() : "",
    vp->xLoc(),
    vp->yLoc(),
    vp->orientStr(),
    vp->xl(),
    vp->yl(),
    vp->xh(),
    vp->yh());
  }

  for (i = 0; i < numWires_; i++) {
    newLayer = 0;
    w = wire(i);
    fprintf(f, "+ %s ", w->wireType());
    for (j = 0; j < w->numPaths(); j++) {
      p = w->path(j);
      p->initTraverse();
      while ((path = (int)(p->next())) != DEFIPATH_DONE) {
         switch (path) {
           case DEFIPATH_LAYER:
                if (newLayer == 0) {
                    fprintf(f, "%s ", p->getLayer());
                    newLayer = 1;
                } else
                    fprintf(f, "NEW %s ", p->getLayer());
                break;
           case DEFIPATH_VIA:
                fprintf(f, "%s\n", p->getVia());
                break;
           case DEFIPATH_VIAROTATION:
                fprintf(f, "%d\n", p->getViaRotation());
                break;
           case DEFIPATH_VIADATA:
                p->getViaData(&numX, &numY, &stepX, &stepY);
                fprintf(f, "%d %d %d %d\n", numX, numY, stepX, stepY);
                break;
           case DEFIPATH_WIDTH:
                fprintf(f, "%d\n", p->getWidth());
                break;
           case DEFIPATH_POINT:
                p->getPoint(&x, &y);
                fprintf(f, "( %d %d )\n", x, y);
                break;
           case DEFIPATH_TAPER:
                fprintf(f, "TAPER\n");
                break;
         }
      }
    }
  }

  if (hasSubnets()) {
    fprintf(f, " Subnets:\n");
    for (i = 0; i < numSubnets(); i++) {
      s = subnet(i);
      s->print(f);
    }
  }

}


void defiNet::bumpName(long long size) {
  if (name_) free(name_);
  name_ = (char*)malloc(size);
  nameSize_ = size;
  name_[0] = '\0';
}


void defiNet::bumpPins(long long size) {
  char** newInstances = (char**)malloc(sizeof(char*)*size);
  char** newPins = (char**)malloc(sizeof(char*)*size);
  char* newMusts = (char*)malloc(size);
  char* newSyn = (char*)malloc(size);
  long long i;

  if (instances_) {
    for (i = 0; i < pinsAllocated_; i++) {
      newInstances[i] = instances_[i];
      newPins[i] = pins_[i];
      newMusts[i] = musts_[i];
      newSyn[i] = synthesized_[i];
    }
    free(instances_);
    free(pins_);
    free(musts_);
    free(synthesized_);
  }

  instances_ = newInstances;
  pins_ = newPins;
  musts_ = newMusts;
  synthesized_ = newSyn;
  pinsAllocated_ = size;
}


void defiNet::bumpProps(long long size) {
  char**  newNames = (char**)malloc(sizeof(char*)*size);
  char**  newValues = (char**)malloc(sizeof(char*)*size);
  double* newDValues = (double*)malloc(sizeof(double)*size);
  char*   newTypes = (char*)malloc(sizeof(char)*size);
  long long i;

  if (propNames_) {
    for (i = 0; i < numProps_; i++) {
      newNames[i] = propNames_[i];
      newValues[i] = propValues_[i];
      newDValues[i] = propDValues_[i];
      newTypes[i] = propTypes_[i];
    }
    free(propNames_);
    free(propValues_);
    free(propDValues_);
    free(propTypes_);
  }

  propNames_ = newNames;
  propValues_ = newValues;
  propDValues_ = newDValues;
  propTypes_ = newTypes;
  propsAllocated_ = size;
}


void defiNet::bumpSubnets(long long size) {
  defiSubnet** newSubnets = (defiSubnet**)malloc(sizeof(defiSubnet*)*size);
  int i;
  if (subnets_) {
    for (i = 0; i < numSubnets_; i++) {
      newSubnets[i] = subnets_[i];
    }
    free(subnets_);
  }

  subnets_ = newSubnets;
  subnetsAllocated_ = size;
}


void defiNet::clear() {
  int i;

  // WMD -- this will be removed by the next release
  isFixed_ = 0;
  isRouted_ = 0;
  isCover_ = 0;

  hasWeight_ = 0;
  hasCap_ = 0;
  hasFrequency_ = 0;
  hasVoltage_ = 0;
  xTalk_ = -1;

  if (vpins_) {
    for (i = 0; i < numVpins_; i++) {
      delete vpins_[i];
    }
    free(vpins_);
    vpins_  = 0;
    numVpins_ = 0;
    vpinsAllocated_ = 0;
  }

  for (i = 0; i < numProps_; i++) {
    free(propNames_[i]);
    free(propValues_[i]);
    propNames_[i] = 0;
    propValues_[i] = 0;
    propDValues_[i] = 0;
  }
  numProps_ = 0;

  for (i = 0; i < numPins_; i++) {
    free(instances_[i]);
    free(pins_[i]);
    instances_[i] = 0;
    pins_[i] = 0;
    musts_[i] = 0;
    synthesized_[i] = 0;
  }
  numPins_ = 0;

  for (i = 0; i < numSubnets_; i++) {
    delete subnets_[i];
    subnets_[i] = 0;
  }
  numSubnets_ = 0;

  if (name_)
     name_[0] = '\0';

  // WMD -- this will be removed by the next release
  if (paths_) {
    for (i = 0; i < numPaths_; i++) {
      delete paths_[i];
    }

    delete [] paths_;
    paths_ = 0;
    numPaths_ = 0;
    pathsAllocated_ = 0;
  }

  // 5.4.1
  fixedbump_ = 0;

  if (source_) { free(source_); source_ = 0; }
  if (pattern_) { free(pattern_); pattern_ = 0; }
  if (original_) { free(original_); original_ = 0; }
  if (use_) { free(use_); use_ = 0; }
  if (nonDefaultRule_) { free(nonDefaultRule_);
            nonDefaultRule_ = 0; }
  style_ = 0;
 
  if (numWires_) {
    for (i = 0; i < numWires_; i++) {
      delete wires_[i];
      wires_[i] = 0;
    }
    free(wires_);
    wires_ = 0;
    numWires_ = 0;
    wiresAllocated_ = 0;
  }

  if (numShields_) {
    for (i = 0; i < numShields_; i++) {
      delete shields_[i];
      shields_[i] = 0;
    }
    numShields_ = 0;
    shieldsAllocated_ = 0;
  }

  if (numNoShields_) {
    for (i = 0; i < numNoShields_; i++) {
      delete shields_[i];
      shields_[i] = 0;
    }
    numNoShields_ = 0;
    shieldsAllocated_ = 0;
  }
  if (shields_)
    free(shields_);

  shields_ = 0;

  if (numWidths_) {
   for (i = 0; i < numWidths_; i++)
     free(wlayers_[i]);
  numWidths_ = 0;
  }

  if (numSpacing_) {
   for (i = 0; i < numSpacing_; i++)
     free(slayers_[i]);
  numSpacing_ = 0;
  }

  if (numShieldNet_) {
   for (i = 0; i < numShieldNet_; i++)
     free(shieldNet_[i]);
   numShieldNet_ = 0;
  }

  clearRectPoly();
  clearVia();
}

void defiNet::clearRectPolyNPath() {
  int i;

  if (paths_) {
    for (i = 0; i < numPaths_; i++) {
      delete paths_[i];
    }
    numPaths_ = 0;
  }

  clearRectPoly();

}

void
defiNet::clearRectPoly()
{
    if (polys_) {
        for (unsigned int i = 0; i < numPolys_; i++) {
            delete polys_[i];
        }

        free(polys_);
    }

    polysAllocated_ = 0;
    numPolys_ = 0;
    polys_ = NULL;

    if (rects_) {
        for (unsigned int i = 0; i < numRects_; i++) {
            delete rects_[i];
        }

        free(rects_);
    }

    rectsAllocated_ = 0;
    numRects_ = 0;
    rects_ = NULL;
}

void
defiNet::clearVia()
{
    if (vias_) {
        for (unsigned int i = 0; i < numVias_; i++) {
            delete vias_[i];
        }

        free(vias_);
    }

    viasAllocated_ = 0;
    numVias_ = 0;
    vias_ = NULL;
}

int defiNet::hasSource() const {
   return source_ ? 1 : 0;
}


int defiNet::hasFixedbump() const {
   return fixedbump_ ? 1 : 0;
}


int defiNet::hasFrequency() const {
  return (int)(hasFrequency_);
}


int defiNet::hasPattern() const {
   return pattern_ ? 1 : 0;
}


int defiNet::hasOriginal() const {
   return original_ ? 1 : 0;
}


int defiNet::hasCap() const {
  return (int)(hasCap_);
}


int defiNet::hasUse() const {
   return use_ ? 1 : 0;
}


int defiNet::hasStyle() const {
   return style_ ? 1 : 0;
}


int defiNet::hasXTalk() const {
   return (xTalk_ != -1) ? 1 : 0;
}


int defiNet::hasNonDefaultRule() const {
   return nonDefaultRule_ ? 1 : 0;
}


void defiNet::setSource(const char* typ) {
  int len;
  if (source_) free(source_);
  len = (int) strlen(typ) + 1;
  source_ = (char*)malloc(len);
  strcpy(source_, defData->DEFCASE(typ));
}


void defiNet::setFixedbump() {
  fixedbump_ = 1;
}


void defiNet::setFrequency(double frequency) {
  frequency_ = frequency;
  hasFrequency_ = 1;
}


void defiNet::setOriginal(const char* typ) {
  int len;
  if (original_) free(original_);
  len = (int) strlen(typ) + 1;
  original_ = (char*)malloc(len);
  strcpy(original_, defData->DEFCASE(typ));
}


void defiNet::setPattern(const char* typ) {
  int len;
  if (pattern_) free(pattern_);
  len = (int) strlen(typ) + 1;
  pattern_ = (char*)malloc(len);
  strcpy(pattern_, defData->DEFCASE(typ));
}


void defiNet::setCap(double w) {
  cap_ = w;
  hasCap_ = 1;
}


void defiNet::setUse(const char* typ) {
  int len;
  if (use_) free(use_);
  len = (int) strlen(typ) + 1;
  use_ = (char*)malloc(len);
  strcpy(use_, defData->DEFCASE(typ));
}


void defiNet::setStyle(int style) {
  style_ = style;
}


void defiNet::setNonDefaultRule(const char* typ) {
  int len;
  if (nonDefaultRule_) free(nonDefaultRule_);
  len = (int) strlen(typ) + 1;
  nonDefaultRule_ = (char*)malloc(len);
  strcpy(nonDefaultRule_, defData->DEFCASE(typ));
}


const char* defiNet::source() const {
  return source_;
}


const char* defiNet::original() const {
  return original_;
}


const char* defiNet::pattern() const {
  return pattern_;
}


double defiNet::cap() const {
  return (hasCap_ ? cap_ : 0.0);
}


double defiNet::frequency() const {
  return (hasFrequency_ ? frequency_ : 0.0);
}


const char* defiNet::use() const {
  return use_;
}


int defiNet::style() const {
  return style_;
}


const char* defiNet::shieldNet(int index) const {
  return shieldNet_[index];
}


const char* defiNet::nonDefaultRule() const {
  return nonDefaultRule_;
}

// WMD -- this will be removed by the next release
void defiNet::bumpPaths(long long size) {
  long long i;

  defiPath** newPaths = new defiPath*[size];
 
  for (i = 0; i < numPaths_; i++)
    newPaths[i] = paths_[i];
 
  delete [] paths_;
  pathsAllocated_ = size;
  paths_ = newPaths;
}
 
// WMD -- this will be removed by the next release
int defiNet::numPaths() const {
  return numPaths_;
}

 
// WMD -- this will be removed by the next release
defiPath* defiNet::path(int index) {
  if (index >= 0 && index < numPaths_)
    return paths_[index];
  return 0;
}


const defiPath* defiNet::path(int index) const {
    if (index >= 0 && index < numPaths_)
        return paths_[index];
    return 0;
}


int defiNet::numWires() const {
  return numWires_;
}


defiWire* defiNet::wire(int index) {
  if (index >= 0 && index < numWires_)
    return wires_[index];
  return 0;
}


const defiWire* defiNet::wire(int index) const {
    if (index >= 0 && index < numWires_)
        return wires_[index];
    return 0;
}


void defiNet::bumpShieldNets(long long size) {
  char** newShieldNets = (char**)malloc(sizeof(char*)*size);
  long long i;
 
  if (shieldNet_) {
    for (i = 0; i < shieldNetsAllocated_; i++) {
      newShieldNets[i] = shieldNet_[i];
    }
    free(shieldNet_);
  }
 
  shieldNet_ = newShieldNets;
  shieldNetsAllocated_ = size;
}


int defiNet::numShields() const {
  return numShields_;
}


defiShield* defiNet::shield(int index) {
  if (index >= 0 && index < numShields_)
    return shields_[index];
  return 0;
}


const defiShield* defiNet::shield(int index) const {
    if (index >= 0 && index < numShields_)
        return shields_[index];
    return 0;
}

int defiNet::numNoShields() const {
  return numNoShields_;
}


defiShield* defiNet::noShield(int index) {
  if (index >= 0 && index < numNoShields_)
    return shields_[index];
  return 0;
}

const defiShield* defiNet::noShield(int index) const {
    if (index >= 0 && index < numNoShields_)
        return shields_[index];
    return 0;
}

int defiNet::hasVoltage() const {
  return (int)(hasVoltage_);
}


double defiNet::voltage() const {
  return voltage_;
}


int defiNet::numWidthRules() const {
  return numWidths_;
}


int defiNet::numSpacingRules() const {
  return numSpacing_;
}


int defiNet::hasWidthRules() const {
  return numWidths_;
}


int defiNet::hasSpacingRules() const {
  return numSpacing_;
}


void defiNet::setXTalk(int i) {
  xTalk_ = i;
}


int defiNet::XTalk() const {
  return xTalk_;
}


void defiNet::addVpin(const char* name) {
  defiVpin* vp;
  if (numVpins_ == vpinsAllocated_) {
    defiVpin** array;
    int i;
    vpinsAllocated_ = vpinsAllocated_ ?
          vpinsAllocated_ * 2 : 2 ;
    array = (defiVpin**)malloc(sizeof(defiVpin*)*vpinsAllocated_);
    for (i = 0; i < numVpins_; i++)
      array[i] = vpins_[i];
    if (vpins_) free(vpins_);
    vpins_ = array;
  }
  vp = vpins_[numVpins_] =  new defiVpin(defData);
  numVpins_ += 1;
  vp->Init(name);
}


void defiNet::addVpinLayer(const char* name) {
  defiVpin* vp = vpins_[numVpins_-1];
  vp->setLayer(name);
}


void defiNet::addVpinLoc(const char* status, int x, int y, int orient) {
  defiVpin* vp = vpins_[numVpins_-1];
  vp->setStatus(*status);
  vp->setLoc(x,y);
  vp->setOrient(orient);
}


void defiNet::addVpinBounds(int xl, int yl, int xh, int yh) {
  defiVpin* vp = vpins_[numVpins_-1];
  vp->setBounds(xl, yl, xh, yh);
}


int defiNet::numVpins() const {
  return numVpins_;
}


defiVpin* defiNet::vpin(int index) {
  if (index < 0 || index >= numVpins_) return 0;
  return vpins_[index];
}


const defiVpin* defiNet::vpin(int index) const {
    if (index < 0 || index >= numVpins_) return 0;
    return vpins_[index];
}

void defiNet::spacingRule(int index, char** layer, double* dist,
     double* left, double* right) const {
  if (index >= 0 && index < numSpacing_) {
    if (layer) *layer = slayers_[index];
    if (dist) *dist = sdist_[index];
    if (left) *left = sleft_[index];
    if (right) *right = sright_[index];
  }
}


void defiNet::widthRule(int index, char** layer, double* dist) const {
  if (index >= 0 && index < numWidths_) {
    if (layer) *layer = wlayers_[index];
    if (dist) *dist = wdist_[index];
  }
}


void defiNet::setVoltage(double v) {
  voltage_ = v;
  hasVoltage_ = 1;
}


void defiNet::setWidth(const char* layer, double d) {
  int len = (int) strlen(layer) + 1;
  char* l = (char*)malloc(len);
  strcpy(l, defData->DEFCASE(layer));

  if (numWidths_ >= widthsAllocated_) {
    int i;
    char** nl;
    double* nd;
    widthsAllocated_ = widthsAllocated_ ?
       widthsAllocated_ * 2 : 4 ;
    nl = (char**)malloc(sizeof(char*) * widthsAllocated_);
    nd = (double*)malloc(sizeof(double) * widthsAllocated_);
    for (i = 0; i < numWidths_; i++) {
      nl[i] = wlayers_[i];
      nd[i] = wdist_[i];
    }
    free(wlayers_);
    free(wdist_);
    wlayers_ = nl;
    wdist_ = nd;
  }

  wlayers_[numWidths_] = l;
  wdist_[numWidths_] = d;
  (numWidths_)++;
}


void defiNet::setSpacing(const char* layer, double d) {
  int len = (int) strlen(layer) + 1;
  char* l = (char*)malloc(len);
  strcpy(l, defData->DEFCASE(layer));

  if (numSpacing_ >= spacingAllocated_) {
    int i;
    char** nl;
    double* nd;
    double* n1;
    double* n2;
    spacingAllocated_ = spacingAllocated_ ?
       spacingAllocated_ * 2 : 4 ;
    nl = (char**)malloc(sizeof(char*) * spacingAllocated_);
    nd = (double*)malloc(sizeof(double) * spacingAllocated_);
    n1 = (double*)malloc(sizeof(double) * spacingAllocated_);
    n2 = (double*)malloc(sizeof(double) * spacingAllocated_);
    for (i = 0; i < numSpacing_; i++) {
      nl[i] = slayers_[i];
      nd[i] = sdist_[i];
      n1[i] = sleft_[i];
      n2[i] = sright_[i];
    }
    free(slayers_);
    free(sdist_);
    free(sleft_);
    free(sright_);
    slayers_ = nl;
    sdist_ = nd;
    sleft_ = n1;
    sright_ = n2;
  }

  slayers_[numSpacing_] = l;
  sdist_[numSpacing_] = d;
  sleft_[numSpacing_] = d;
  sright_[numSpacing_] = d;
  (numSpacing_)++;
}


void defiNet::setRange(double left, double right) {
  // This is always called right after setSpacing.
  sleft_[numSpacing_-1] = left;
  sright_[numSpacing_-1] = right;
}



// *****************************************************************************
// This function adds the parsed SPECIALNET POLYGON to the cache 'polys_'.
// *****************************************************************************
void
defiNet::addPolygon(defiNetPoly *poly,
                    int         *needCbk)
{
    if (numPolys_ == polysAllocated_) {
        if (polysAllocated_ == 0) {
            int INITIAL_SIZE = 1000;

            polysAllocated_ = INITIAL_SIZE;
        } else {
            polysAllocated_ *= 2;
        }

        defiNetPoly **newPolys;

        newPolys = (defiNetPoly**)malloc(sizeof(void*) * polysAllocated_);

        for (int i = 0; i < numPolys_; i++) {
            newPolys[i] = polys_[i];
        }

        free(polys_);
        polys_ = newPolys;
    }

    polys_[numPolys_] = poly;
    numPolys_ += 1;

    // Want to invoke the partial callback if set.
    *needCbk = (numPolys_ == 1000);
}



// *****************************************************************************
// This function returns the number of SPECIALNETS POLYGONs parsed.
// *****************************************************************************
int defiNet::numPolygons() const {
  return numPolys_;
}



// *****************************************************************************
// This function validates 'index' and returns a pointer to the defiNetPoly
// object, stored at that index in the 'polys_' cache.
// *****************************************************************************
defiNetPoly*
defiNet::getPoly(int index) const
{
    if (index < 0 || index > numPolys_) {
        char errMsg[128];

        sprintf (errMsg,
                 "ERROR (DEFPARS-6085): The index number %d specified for the NET POLYGON is invalid.\nValid index is from 0 to %d. Specify a valid index number and then try again.",
                 index, numPolys_);
        defiError(0, 6085, errMsg, defData);

        return NULL;
    }

    return polys_[index];
}



// *****************************************************************************
// Accessor functions to retrieve information about SPECIALNETS POLYGON
// indexed at 'index'.
// *****************************************************************************
const char*
defiNet::polygonName(int index) const
{
    defiNetPoly *poly = getPoly(index);

    return poly ? poly->getName() : NULL;
}

const char*
defiNet::polyRouteStatus(int index) const
{
    defiNetPoly *poly = getPoly(index);

    return poly ? poly->getRouteStatus() : NULL;
}

int 
defiNet::hasPolyRouteStatusShield(int index) const
{
    defiNetPoly *poly = getPoly(index);

    return (poly && poly->getRouteStatusShieldName()) ? 1 : 0;
}

const char* 
defiNet::polyRouteStatusShieldName(int index) const
{
    defiNetPoly *poly = getPoly(index);

    return poly ? poly->getRouteStatusShieldName() : NULL;
}

const char* 
defiNet::polyShapeType(int index) const
{
    if (defiNetPoly *poly = getPoly(index)) {
        const char *shapeType = poly->getShapeType();

        return shapeType ? shapeType : "";
    }

    return NULL;
  
}

int
defiNet::numPolyProps(int index) const
{
    if (defiNetPoly *poly = getPoly(index)) {
        if (const defrProps *props = poly->getProps()) {
            return props->size();
        }
    }

    return 0;
}

const defiProp*
defiNet::polyProp(int index,
                  int propIdx) const
{
    if (defiNetPoly *poly = getPoly(index)) {
        if (const defrProps *props = poly->getProps()) {
            return (*props)[propIdx];
        }
    }

    return NULL;
}

int
defiNet::polyMask(int index) const
{
    defiNetPoly *poly = getPoly(index);

    return poly ? poly->getMask() : 0;
}

defiPoints
defiNet::getPolygon(int index) const
{
    defiNetPoly *poly = getPoly(index);

    return poly ? poly->getPoints() : defiPoints();
}



// *****************************************************************************
// This function adds the parsed SPECIALNET RECT to the cache 'rects_'.
// *****************************************************************************
void
defiNet::addRect(defiNetRect  *rect,
                 int          *needCbk)
{
    if (numRects_ == rectsAllocated_) {
        if (rectsAllocated_ == 0) {
            int INITIAL_SIZE = 1000;

            rectsAllocated_ = INITIAL_SIZE;
        } else {
            rectsAllocated_ *= 2;
        }

        defiNetRect **newRects;

        newRects = (defiNetRect**)malloc(sizeof(void*) * rectsAllocated_);

        for (int i = 0; i < numRects_; i++) {
            newRects[i] = rects_[i];
        }

        free(rects_);
        rects_ = newRects;
    }

    rects_[numRects_] = rect;
    numRects_ += 1;

    // Want to invoke the partial callback if set.
    *needCbk = (numRects_ == 1000);
}



// *****************************************************************************
// This function returns the number of SPECIALNETS RECTs parsed.
// *****************************************************************************
int defiNet::numRectangles() const {
  return numRects_;
}



// *****************************************************************************
// This function validates 'index' and returns a pointer to the defiNetRect
// object, stored at that index in the 'rects_' cache.
// *****************************************************************************
defiNetRect*
defiNet::getRect(int index) const
{
    if (index < 0 || index > numRects_) {
        char errMsg[128];

        sprintf (errMsg,
                 "ERROR (DEFPARS-6086): The index number %d specified for the NET RECTANGLE is invalid.\nValid index is from 0 to %d. Specify a valid index number and then try again.",
                 index, numRects_);
        defiError(0, 6086, errMsg, defData);

        return NULL;
    }

    return rects_[index];
}



// *****************************************************************************
// Accessor functions to retrieve information about SPECIALNETS RECT
// indexed at 'index'.
// *****************************************************************************
const char*
defiNet::rectName(int index) const
{
    defiNetRect *rect = getRect(index);

    return rect ? rect->getName() : NULL;
}

const char*
defiNet::rectRouteStatus(int index) const
{
    defiNetRect *rect = getRect(index);

    return rect ? rect->getRouteStatus() : NULL;
}

int
defiNet::hasRectRouteStatusShield(int index) const
{
    defiNetRect *rect = getRect(index);

    return (rect && rect->getRouteStatusShieldName()) ? 1 : 0;
}

const char* 
defiNet::rectRouteStatusShieldName(int index) const
{
    defiNetRect *rect = getRect(index);

    return rect ? rect->getRouteStatusShieldName() : NULL;
}

const char* 
defiNet::rectShapeType(int index) const
{
    if (defiNetRect *rect = getRect(index)) {
        const char *shapeType = rect->getShapeType();

        return shapeType ? shapeType : "";
    }

    return NULL;
}

int
defiNet::numRectProps(int index) const
{
    if (defiNetRect *rect = getRect(index)) {
        if (const defrProps *props = rect->getProps()) {
            return props->size();
        }
    }

    return 0;
}

const defiProp*
defiNet::rectProp(int index,
                  int propIdx) const
{
    if (defiNetRect *rect = getRect(index)) {
        if (const defrProps *props = rect->getProps()) {
            return (*props)[propIdx];
        }
    }

    return NULL;
}

int
defiNet::xl(int index) const
{
    defiNetRect *rect = getRect(index);

    return rect ? rect->getXl() : 0;
}

int
defiNet::yl(int index) const
{
    defiNetRect *rect = getRect(index);

    return rect ? rect->getYl() : 0;
}

int
defiNet::xh(int index) const
{
    defiNetRect *rect = getRect(index);

    return rect ? rect->getXh() : 0;
}

int
defiNet::yh(int index) const
{
    defiNetRect *rect = getRect(index);

    return rect ? rect->getYh() : 0;
}

int
defiNet::rectMask(int index) const
{
    defiNetRect *rect = getRect(index);

    return rect ? rect->getMask() : 0;
}



// *****************************************************************************
// This function adds the parsed SPECIALNET VIA to the cache 'vias_'.
// *****************************************************************************
void
defiNet::addVia(defiNetVia  *via,
                int         *needCbk)
{
    if (numVias_ == viasAllocated_) {
        if (viasAllocated_ == 0) {
            int INITIAL_SIZE = 1000;

            viasAllocated_ = INITIAL_SIZE;
        } else {
            viasAllocated_ *= 2;
        }

        defiNetVia **newVias;

        newVias = (defiNetVia**)malloc(sizeof(void*) * viasAllocated_);

        for (int i = 0; i < numVias_; i++) {
            newVias[i] = vias_[i];
        }

        free(vias_);
        vias_ = newVias;
    }

    vias_[numVias_] = via;
    numVias_ += 1;

    // Want to invoke the partial callback if set.
    *needCbk = (numVias_ == 1000);
}



// *****************************************************************************
// This function returns the number of SPECIALNETS VIAs parsed.
// *****************************************************************************
int
defiNet::numViaSpecs() const
{
    return numVias_;
}



// *****************************************************************************
// This function validates 'index' and returns a pointer to the defiNetVia
// object, stored at that index in the 'vias_' cache.
// *****************************************************************************
defiNetVia*
defiNet::getVia(int index) const
{
    if (index < 0 || index > numVias_) {
        char errMsg[128];
        sprintf (errMsg,
                 "ERROR (DEFPARS-6085): The index number %d specified for the NET VIA is invalid.\nValid index is from 0 to %d. Specify a valid index number and then try again.",
                 index, numVias_);
        defiError(0, 6085, errMsg, defData);

        return NULL;
    }

    return vias_[index];
}



// *****************************************************************************
// Accessor functions to retrieve information about SPECIALNETS VIA
// indexed at 'index'.
// *****************************************************************************
const char*
defiNet::viaName(int index) const
{
    defiNetVia *via = getVia(index);

    return via ? via->getName() : NULL;
}

const char*
defiNet::viaRouteStatus(int index) const
{
    defiNetVia *via = getVia(index);

    return via ? via->getRouteStatus() : NULL;
}

int 
defiNet::hasViaRouteStatusShield(int index) const
{
    defiNetVia *via = getVia(index);

    return (via && via->getRouteStatusShieldName()) ? 1 : 0;
}

const char* 
defiNet::viaRouteStatusShieldName(int index) const
{
    defiNetVia *via = getVia(index);

    return via ? via->getRouteStatusShieldName() : NULL;
}

const char* 
defiNet::viaShapeType(int index) const
{
    if (defiNetVia *via = getVia(index)) {
        const char *shapeType = via->getShapeType();

        return shapeType ? shapeType : "";
    }

    return NULL;
}

int
defiNet::numViaProps(int index) const
{
    if (defiNetVia *via = getVia(index)) {
        if (const defrProps *props = via->getProps()) {
            return props->size();
        }
    }

    return 0;
}

const defiProp*
defiNet::viaProp(int  index,
                 int  propIdx) const
{
    if (defiNetVia *via = getVia(index)) {
        if (const defrProps *props = via->getProps()) {
            return (*props)[propIdx];
        }
    }

    return NULL;
}

int
defiNet::viaOrient(int index) const
{
    defiNetVia *via = getVia(index);

    return via ? via->getOrient() : 0;
}

const char*
defiNet::viaOrientStr(int index) const
{
    defiNetVia *via = getVia(index);

    return via ? defiOrientStr(via->getOrient()) : NULL;
}

int
defiNet::topMaskNum(int index) const
{
    defiNetVia *via = getVia(index);
    
    return via ? via->getTopMaskNum() : 0;
}

int
defiNet::cutMaskNum(int index) const
{
    defiNetVia *via = getVia(index);
    
    return via ? via->getCutMaskNum() : 0;
}

int
defiNet::bottomMaskNum(int index) const
{
    defiNetVia *via = getVia(index);
    
    return via ? via->getBottomMaskNum() : 0;
}

defiPoints
defiNet::getViaPts(int index) const
{
    defiNetVia *via = getVia(index);
    
    return via ? via->getPoints() : defiPoints();
}

END_LEFDEF_PARSER_NAMESPACE

