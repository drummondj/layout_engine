// *****************************************************************************
// *****************************************************************************
// Copyright 2013, Cadence Design Systems
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
//  $Date: 2025/03/27 $
//  $State:  $
// *****************************************************************************
// *****************************************************************************

#ifndef defiFILL_h
#define defiFILL_h

#include <stdio.h>
#include <vector>

#include "defiKRDefs.hpp"
#include "defiMisc.hpp"

BEGIN_LEFDEF_PARSER_NAMESPACE

class defrData;

struct defiFillRect {
                            defiFillRect(int xl, int yl, int xh, int yh);

    int                     m_xl;
    int                     m_yl;
    int                     m_xh;
    int                     m_yh;
};

struct defiFillPoly {
                            defiFillPoly(int numPts, const int *x, const int *y);
                            defiFillPoly(defiFillPoly&& other) noexcept;
                            ~defiFillPoly();
    
    defiPoints              m_pts;
};

class defiFill {
public:
  defiFill(defrData *data);
  void Init();

  void Destroy();
  ~defiFill();

  void clear();
  void clearShapes();
  void clearPts();

  void setLayer(const char* name);
  void setLayerOpc();                             // 5.7
  void addRect(int xl, int yl, int xh, int yh);
  void addPolygon(defiGeometries* geom);
  void addLayerProp(defiProp* prop);              // 6.0
  void setVia(const char* name);                  // 5.7
  void setViaOpc();                               // 5.7
  void addPts(defiGeometries*   geom, 
              defrInts*         orients);  // 5.7 (orient 6.0)
  void addViaProp(defiProp* prop);                // 6.0

  int hasLayer() const;
  const char* layerName() const;
  int hasLayerOpc() const;                        // 5.7

  void setMask(int colorMask);			  // 5.8
  int layerMask() const;                          // 5.8
  int viaTopMask() const;			  // 5.8
  int viaCutMask() const;			  // 5.8
  int viaBottomMask() const;                      // 5.8
  int numLayerProps() const;
  defiProp* layerProp(int index) const;

  size_t                    numRectangles() const;
  int                       xl(size_t index) const;
  int                       yl(size_t index) const;
  int                       xh(size_t index) const;
  int                       yh(size_t index) const;
  const defiFillRect        &rect(size_t index) const;

  size_t                    numPolygons() const;  // 5.6
  const defiPoints          &getPolygon(size_t index) const;  // 5.6

  int hasVia() const;                             // 5.7
  const char* viaName() const;                    // 5.7
  int hasViaOpc() const;                          // 5.7

  int numViaPts() const;                          // 5.7
  struct defiPoints getViaPts(int index) const;   // 5.7
  int getViaOrient(int index, int viaIdx) const;  // 6.0
  int numViaProps() const;                     // 6.0
  const defiProp*   viaProp(int index) const;  // 6.0

  void print(FILE* f) const;

protected:
  int   hasLayer_;
  char* layerName_;
  int   layerNameLength_;
  int   layerOpc_;                  // 5.7

  std::vector<defiFillRect> m_rects;
  std::vector<defiFillPoly> m_polys;

  int   hasVia_;                    // 5.7
  char* viaName_;                   // 5.7
  int   viaNameLength_;             // 5.7
  int   viaOpc_;                    // 5.7
  int   numPts_;                    // 5.7
  int   ptsAllocated_;              // 5.7
  int   mask_;                      // 5.8
  struct defiPoints** viaPts_;      // 5.7

  defrIntsInts*       viaOrients_;
  defrProps*          layerProps_;
  defrProps*          viaProps_;

  defrData *defData;
};


END_LEFDEF_PARSER_NAMESPACE

USE_LEFDEF_PARSER_NAMESPACE

#endif
