#include "pcError.h"
#include <MeshSimAdapt.h>
#include <SimUtil.h>
#include <SimPartitionedMesh.h>
#include "SimMeshMove.h"
#include "SimMeshTools.h"
#include "apfSIM.h"
#include "gmi_sim.h"
#include "apfShape.h"
#include <PCU.h>
#include <cassert>
#include <phastaChef.h>
#include "pcAdapter.h"

namespace pc {

  double getShortestEdgeLength(apf::Mesh* m, apf::MeshEntity* elm) {
    int useFirFlag = 1;
    double min = 0.0;
    apf::Downward edges;
    int nd = m->getDownward(elm, 1, edges);
    for (int i=0; i < nd; ++i) {
      double el = apf::measure(m,edges[i]);
      if (useFirFlag) {
        min = el;
        useFirFlag = 0;
      }
      else {
        if (el < min) min = el;
      }
    }
    return min;
  }

  void calAndAttachVMSSizeField(apf::Mesh2*& m, ph::Input& in, phSolver::Input& inp) {
    pc::attachCurrentSizeField(m);
    apf::Field* cur_size = m->findField("cur_size");
    assert(cur_size);

    //read phasta element-based field VMS_error
    apf::Field* err = m->findField("VMS_error");
    //get nodal-based mesh size field
    apf::Field* sizes = m->findField("sizes");

    //SS:read phasta solution field
    // apf::Field* sol = m->findField("solution");
    // //SS:read shock detection field (1s or 0s)
    apf::Field* shock_param = m->findField("Shock Param");

    //create a field to store element-based mesh size
    int nsd = m->getDimension();
    apf::Field* elm_size = apf::createField(m, "elm_size", apf::SCALAR, apf::getConstant(nsd));

    //get desired error
    //currently, we only focus on the momemtum error // debugging
    assert((string)inp.GetValue("Error Trigger Equation Option") == "Momentum");
    double desr_err[3];
    desr_err[0] = (double)inp.GetValue("Target Error for Mass Equation");
    desr_err[1] = (double)inp.GetValue("Target Error for Momentum Equation");
    desr_err[2] = (double)inp.GetValue("Target Error for Energy Equation");

    //get parameter
    double exp_m = 0.0;
    if((string)inp.GetValue("Error Estimation Option") == "H1norm") {
      exp_m = 1.0;
    }
    else if ((string)inp.GetValue("Error Estimation Option") == "L2norm") {
      exp_m = 0.0;
    }

    //loop over elements
    apf::NewArray<double> curr_err(apf::countComponents(err));
    apf::MeshEntity* elm;
    apf::MeshIterator* it = m->begin(nsd);
    while ((elm = m->iterate(it))) {
      double h_old = 0.0;
      double h_new = 0.0;
      //get old size
      h_old = apf::getScalar(cur_size, elm, 0);
      //get error
      apf::getComponents(err, elm, 0, &curr_err[0]);
      //get new size
      //currently, we only focus on the momemtum error // debugging
      double factor = 0.0;
      double curr_err_mag = sqrt(curr_err[1]*curr_err[1]
                                +curr_err[2]*curr_err[2]
                                +curr_err[3]*curr_err[3]);
      factor = desr_err[1] / curr_err_mag;
      // factor = desr_err[1] / sqrt(curr_err[1]*curr_err[1]
      //                            +curr_err[2]*curr_err[2]
      //                            +curr_err[3]*curr_err[3]);
      if(!isfinite(factor)) factor = 1e16; // avoid inf and NaN
      h_new = h_old/sqrt(3) * pow(factor, 2.0/(2.0*(1.0+1.0-exp_m)+(double)nsd));
      
      //testing only changing size of elements on shock
      bool test_shock_param = true;
      if(test_shock_param){
        // apf::Adjacent Adja;
        // m->getAdjacent(elm,0,Adja);
        double n_shock = 0.0; // neighboring element with shock
        apf::Adjacent adj_vert;
        m->getAdjacent(elm, 0, adj_vert);
        for(size_t i = 0; i < adj_vert.getSize(); i++){
          apf::Adjacent adj_elm;
          m->getAdjacent(adj_vert[i],m->getDimension(), adj_elm);
          for(size_t j= 0; j<adj_elm.getSize(); j++){
            n_shock += apf::getScalar(shock_param,adj_elm[j], 0);
          }
        }

        double param = apf::getScalar(shock_param, elm, 0);
        if(n_shock > 0 || param > 0.0) h_new = 7.5e-5;
      }
      //set new size
      apf::setScalar(elm_size, elm, 0, h_new);
    }
    m->end(it);

    apf::Field* position = m->findField("motion_coords");

    //loop over vertices
    apf::MeshEntity* vtx;
    it = m->begin(0);
    while ((vtx = m->iterate(it))) {
      apf::Adjacent adj_elm;
      m->getAdjacent(vtx, m->getDimension(), adj_elm);
      double weightedSize = 0.0;
      double totalError = 0.0;
      double current_size = 0.0;
      //loop over adjacent elements
      bool shock = false;
      for (std::size_t i = 0; i < adj_elm.getSize(); ++i) {
        //get weighted size and weight
        apf::getComponents(err, adj_elm[i], 0, &curr_err[0]);
        double curr_size = apf::getScalar(elm_size, adj_elm[i], 0);
        //current_size += curr_size;

        double param = apf::getScalar(shock_param, adj_elm[i], 0);  
        if(param > 0.5) shock = true;

        //currently, we only focus on the momemtum error // debugging
//        weightedSize += apf::getScalar(elm_size,adj_elm[i],0)*curr_err[1];
        weightedSize += curr_size*sqrt(curr_err[1]*curr_err[1]
                                      +curr_err[2]*curr_err[2]
                                      +curr_err[3]*curr_err[3]);
        totalError += sqrt(curr_err[1]*curr_err[1]
                          +curr_err[2]*curr_err[2]
                          +curr_err[3]*curr_err[3]);

      }
      //get size of this vertex
      weightedSize = weightedSize / totalError;

      //set new size
      apf::Vector3 v_mag;
      if(!isfinite(weightedSize)) weightedSize = 1e16; // avoid inf and NaN

      apf::Vector3 pos;
      apf::getComponents(position,vtx,0,&pos[0]);

      double h_max = 1.5/16;
      double h_min = 1.5/128;

      double L1 = (-.75+-.2)/2; 
      double delta = 0.1;

      if( pos[1] < L1 + delta && pos[1] > L1-delta ){
        v_mag[0] = h_max;
        v_mag[1] = h_min;
        v_mag[2] = h_max;
      }
      else{
        v_mag[0] = h_max;
        v_mag[1] = h_max;
        v_mag[2] = h_max;
      }
 

        //v_mag[0] = weightedSize*3;
        //v_mag[1] = weightedSize/20;
        //v_mag[2] = weightedSize*3;

      apf::setVector(sizes, vtx, 0, v_mag);
    }
    m->end(it);

    //delete element-based error and mesh size
    apf::destroyField(cur_size);
    apf::destroyField(elm_size);
  }

  void attachVMSSizeField(apf::Mesh2*& m, ph::Input& in, phSolver::Input& inp) {
    // make sure we have VMS error field and newly-created size field
    assert(m->findField("VMS_error"));
    assert(m->findField("sizes"));
    calAndAttachVMSSizeField(m, in, inp);
  }
}
