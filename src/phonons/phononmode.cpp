/**
 * @file src/phononmode.cpp
 *
 * @brief 
 *
 * @author Jordan Bieder <jordan.bieder@cea.fr>
 *
 * @copyright Copyright 2014 Jordan Bieder
 *
 * This file is part of Agate.
 *
 * Agate is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Agate is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Agate.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "phonons/phononmode.hpp"
#include "base/exception.hpp"
#include "base/utils.hpp"
#include "base/mendeleev.hpp"
#include "base/phys.hpp"
#include "base/geometry.hpp"
#include <cmath>
#include <iostream>
#include <typeinfo>

using namespace Agate;

//
PhononMode::PhononMode() :
  _natom(0),
  _hasASR(false),
#ifdef HAVE_EIGEN
  _qpt(),
  _d2cart(),
  _eigenVec(),
  _eigenDisp(),
  _frequencies(),
  _gprim(),
  _rprim(),	
  _asr(),
  _zeff(),  
#endif
  _mass()
{
#ifndef HAVE_EIGEN
  (void) (_hasASR);
  throw EXCEPTION("To use this functionnality you need to compile with EIGEN support",ERRABT);
#endif
}


//
PhononMode::PhononMode(unsigned natom) :
  _natom(natom),
  _hasASR(false),
#ifdef HAVE_EIGEN
  _qpt(),
  _d2cart(3*natom,3*natom),
  _eigenVec(3*natom,3*natom),
  _eigenDisp(3*natom,3*natom),
  _frequencies(),
  _gprim(),
  _rprim(),
  _asr(),
  _zeff(),
#endif
  _mass(natom)
{
#ifndef HAVE_EIGEN
  throw EXCEPTION("To use this functionnality you need to compile with EIGEN support",ERRABT);
#endif
}


//
PhononMode::~PhononMode() {
  ;
}


//
void PhononMode::resize(const unsigned natom) {
  _natom = natom;
  // resize matrix
#ifdef HAVE_EIGEN
  _d2cart.resize(3*_natom,3*_natom);
  _eigenVec.resize(3*_natom,3*natom);
  _eigenDisp.resize(3*_natom,3*natom);
  _frequencies.resize(3*_natom);
  _zeff.resize(_natom);
#endif 
  _mass.resize(_natom);
}

//
void PhononMode::computeForceCst(const geometry::vec3d& qpt, const Ddb& ddb) {
  this->resize(ddb.natom());

#ifdef HAVE_EIGEN
  const geometry::mat3d gprim = ddb.gprim();
  _gprim << gprim[0], gprim[1],gprim[2],
         gprim[3], gprim[4], gprim[5],
         gprim[6], gprim[7], gprim[8];

  // Initialize qpt
  _qpt << qpt[0], qpt[1], qpt[2];
#endif

  bool isNormalized = ddb.isNormalized();
  for ( unsigned iatom = 0 ; iatom < _natom ; ++iatom ) {
    _mass[iatom] = (isNormalized?1:MendeTable.mass[ddb.znucl().at(ddb.typat().at(iatom)-1)]*phys::amu_emass); // type starts at 1
  }

  try {
    this->computeForceCst(ddb.getDdb(qpt));
  }
  catch (Exception& e) {
    e.ADD("Abording",ERRDIV);
    throw e;
  }
}


/*Calculate Linear Repsonse of Phonons to a static dielectric field from DFPT*/
std::vector<double> PhononMode::lin_res(const geometry::vec3d &E_vec, double E_Amp, const Ddb& ddb) {
#ifndef HAVE_EIGEN
  throw EXCEPTION("To use this functionnality you need to compile with EIGEN support",ERRABT);
  (void) E_vec;
  (void) E_Amp;
  (void) ddb;
#else
	/*---- Get necessary Data ---*/
	
	//double E_Amp;
	//std::vector<double> E_vec(3);
        //E_vec = {1,0,0}; 
	//E_Amp = 10000; 
	
	_natom = ddb.natom();  					/// get natom 
  _zeff.resize(_natom);
  for ( unsigned iatom = 0 ; iatom < _natom ; ++iatom )
    _zeff[iatom] = ddb.getZeff(iatom); 		/// get BEC-Tensors

	const geometry::mat3d rprim = ddb.rprim(); 			/// get rprim
 	 _rprim << rprim[0], rprim[1],rprim[2],
         rprim[3], rprim[4], rprim[5],
         rprim[6], rprim[7], rprim[8];
	double cell_V; 						/// get unit cell volume (bohr^3)
	cell_V = geometry::det(rprim);
	cell_V = cell_V*phys::b2A*phys::b2A*phys::b2A;
       	
	/*Following lines: get Eigenfrequencies and Modes at Gamma */
	PhononMode gamma(_natom);
	gamma.computeASR(ddb);
	gamma.computeForceCst({{0,0,0}},ddb);
	std::vector<double> freq_gamma(3*_natom);
	std::vector<complex> disp_gamma(3*_natom*3*_natom);
	gamma.computeEigen(&freq_gamma[0],&disp_gamma[0]);
	for (unsigned m = 0; m < freq_gamma.size(); ++m){	/// Transform mode energies from Ha in THz                
		freq_gamma[m] = phys::Ha2THz * freq_gamma[m];	
		if (freq_gamma[m] < -1)
			throw EXCEPTION("NEGATIVE PHONON FREQUENCY FOUND: The linear response to an Electric-Field calculation makes only sense in stable structures. Fully relax your structure",ERRDIV);	
	}
  _mass.resize(_natom);
  for ( unsigned iatom = 0 ; iatom < _natom ; ++iatom ) {
    _mass[iatom] = MendeTable.mass[ddb.znucl().at(ddb.typat().at(iatom)-1)]; // type starts at 1
  }	
  for ( unsigned i = 0; i < disp_gamma.size(); ++i ) {             
    disp_gamma[i] = disp_gamma[i]*sqrt(phys::amu_emass);
  }

	/*--------- Calculate diplacement under electric filed ---------*/ 
	/* 1. Calculate polarity of each mode and store*/
	/*** organization pol[i] = p(olarity)_m(ode)_(direction)x, p_m_y, p_m_z, p_m+1_x...*/	 
	std::vector<double> pol(3*freq_gamma.size());
	for (unsigned m = 3; m < freq_gamma.size(); ++m) { 
		for ( unsigned alpha = 0; alpha < 3; ++alpha) {
			for (unsigned i = 0; i < _natom; ++i) { 
				for(unsigned gamma = 0; gamma < 3; ++gamma) {				
				pol[m*3+alpha] = pol[m*3+alpha] + _zeff[i][geometry::mat3dind( alpha+1, gamma+1)] * disp_gamma[(m*3*_natom) + 3*i + gamma].real(); 	
				}
			}		
		} 
	}
	/*2. Calculate Displacements Matrix under Electric Field */ 
	/*** Size of Matrix tau = 9*iatom ***/ 
	/*** Organisatzion tau = d(isp)_(atom)i_(direc)_x_E(field)x, d_i_y_Ex, d_i_z_Ex, d_i_x_Ey... ***/ 
	std::vector<double> tau(9*_natom);
	for (unsigned m = 0	; m < freq_gamma.size(); ++m) { 
		for (unsigned i = 0; i < _natom; ++i) {
			for ( unsigned E_al = 0; E_al < 3; ++E_al) {			 
			tau[9*i+3*E_al+0] = tau[9*i+3*E_al+0] + phys::fac*pol[3*m + E_al]*disp_gamma[(m*3*_natom) + 3*i + 0].real()/(1e05*freq_gamma[m]*freq_gamma[m]);
			tau[9*i+3*E_al+1] = tau[9*i+3*E_al+1] + phys::fac*pol[3*m + E_al]*disp_gamma[(m*3*_natom) + 3*i + 1].real()/(1e05*freq_gamma[m]*freq_gamma[m]);
			tau[9*i+3*E_al+2] = tau[9*i+3*E_al+2] + phys::fac*pol[3*m + E_al]*disp_gamma[(m*3*_natom) + 3*i + 2].real()/(1e05*freq_gamma[m]*freq_gamma[m]);				
			}
		}
	}

	/*3. Calculate Real Space displacement in Angstrom  */ 
	/*			E_field * tau 		    */
	std::vector<double> disp_E(3*_natom);
	
	for (unsigned i = 0 ; i < _natom; ++i){ 
		disp_E[3*i + 0] = phys::A2b*(E_Amp*E_vec[0]*tau[9*i + 0] + E_Amp*E_vec[1]*tau[9*i + 3] + E_Amp*E_vec[2]*tau[9*i + 6]);  
		disp_E[3*i + 1] = phys::A2b*(E_Amp*E_vec[0]*tau[9*i + 1] + E_Amp*E_vec[1]*tau[9*i + 4] + E_Amp*E_vec[2]*tau[9*i + 7]);
		disp_E[3*i + 2] = phys::A2b*(E_Amp*E_vec[0]*tau[9*i + 2] + E_Amp*E_vec[1]*tau[9*i + 6] + E_Amp*E_vec[2]*tau[9*i + 8]);  
	}
	
	/*   Calculate vibronic dielectric constant  */
        /*Currently not used but kept if ever needed */ 
	geometry::mat3d diel; 
	for (unsigned m = 3; m < freq_gamma.size(); ++m) { 
		diel[geometry::mat3dind( 1, 1)] = diel[geometry::mat3dind( 1, 1)] + pol[m*3]*pol[m*3]/(freq_gamma[m]*freq_gamma[m]);
		diel[geometry::mat3dind( 1, 2)] = diel[geometry::mat3dind( 1, 2)] + pol[m*3]*pol[m*3+1]/(freq_gamma[m]*freq_gamma[m]);
		diel[geometry::mat3dind( 1, 3)] = diel[geometry::mat3dind( 1, 3)] + pol[m*3]*pol[m*3+2]/(freq_gamma[m]*freq_gamma[m]);

		diel[geometry::mat3dind( 2, 1)] = diel[geometry::mat3dind( 2, 1)] + pol[m*3+1]*pol[m*3]/(freq_gamma[m]*freq_gamma[m]);
		diel[geometry::mat3dind( 2, 2)] = diel[geometry::mat3dind( 2, 2)] + pol[m*3+1]*pol[m*3+1]/(freq_gamma[m]*freq_gamma[m]);
		diel[geometry::mat3dind( 2, 3)] = diel[geometry::mat3dind( 2, 3)] + pol[m*3+1]*pol[m*3+2]/(freq_gamma[m]*freq_gamma[m]);

		diel[geometry::mat3dind( 3, 1)] = diel[geometry::mat3dind( 3, 1)] + pol[m*3+2]*pol[m*3]/(freq_gamma[m]*freq_gamma[m]);
		diel[geometry::mat3dind( 3, 2)] = diel[geometry::mat3dind( 3, 2)] + pol[m*3+2]*pol[m*3+1]/(freq_gamma[m]*freq_gamma[m]);
		diel[geometry::mat3dind( 3, 3)] = diel[geometry::mat3dind( 3, 3)] + pol[m*3+2]*pol[m*3+2]/(freq_gamma[m]*freq_gamma[m]);	
	} 
        diel = geometry::sc_mult(diel, phys::fac/(phys::Eps_0 * cell_V));
	
	/*4. returen displacement under electric field */ 
	return disp_E;
#endif
}

 
//
void PhononMode::computeForceCst(const std::vector<Ddb::d2der>& ddb) {
  if ( _natom == 0 )
    throw EXCEPTION("You must initialize the number of atom (natom) before setting the dynamical matrix",ERRDIV);

  if ( _natom*_natom*9 > ddb.size() )
    throw EXCEPTION(std::string("Size mismatche in ddb: expected at least ")
        + utils::to_string(_natom*_natom*9)
        + std::string(" elements and got ")
        + utils::to_string(ddb.size()),ERRDIV);

  /*
  bool efield = ( ddb.size() == (_natom+1)*(_natom+1)*9);
  if ( !efield && _natom*_natom*9 != ddb.size() )
    throw EXCEPTION(std::string("Unexpected number of elemets in ddb: expecting ")
        + utils::to_string(_natom*_natom*9)
        + std::string(" or ")
        + utils::to_string((_natom+1)*(_natom+1)*9)
        + std::string(" elements and got ")
        + utils::to_string(ddb.size()),ERRDIV);
        */

#ifdef HAVE_EIGEN
  Eigen::Matrix<short,Eigen::Dynamic,Eigen::Dynamic> check;
  check.resize(3*_natom,3*_natom);
  check.setOnes();
  // First loop : Construct and first basis change from reduced to cartesian (lines)
  for ( auto& elt : ddb ) {
    const unsigned idir1 = elt.first[0];
    const unsigned ipert1 = elt.first[1];
    const unsigned idir2 = elt.first[2];
    const unsigned ipert2 = elt.first[3];
    if ( !(idir1 < 3 && idir2 < 3 && ipert1 < _natom && ipert2 < _natom) ) continue;
    _d2cart(ipert2*3+idir2,ipert1*3+idir1) = elt.second;
      check(ipert2*3+idir2,ipert1*3+idir1) = false;
  }
  if ( check.any() )
    throw EXCEPTION("Missing some data in the DDB",ERRDIV);

  for ( unsigned ipert2 = 0 ; ipert2 < _natom ; ++ipert2 ) {
    for ( unsigned idir2 = 0 ; idir2 < 3 ; ++idir2 ) {
      for ( unsigned ipert1 = 0 ; ipert1 < _natom ; ++ipert1 ) {
        Eigen::Vector3cd d2red;
        d2red[0] = _d2cart(ipert2*3+idir2,ipert1*3  );
        d2red[1] = _d2cart(ipert2*3+idir2,ipert1*3+1);
        d2red[2] = _d2cart(ipert2*3+idir2,ipert1*3+2);
        const Eigen::Vector3cd d2cart(_gprim * d2red);
        _d2cart(ipert2*3+idir2,ipert1*3  ) = d2cart[0];
        _d2cart(ipert2*3+idir2,ipert1*3+1) = d2cart[1];
        _d2cart(ipert2*3+idir2,ipert1*3+2) = d2cart[2];
        //if ( efield && ipert1 == _natom-1 ) ind+=3; //
      }
    }
  }
  //Second loop : change basis from reduced to cartesian (columns)
  for ( unsigned ipert2 = 0 ; ipert2 < _natom ; ++ipert2 ) {
    for ( unsigned ipert1 = 0 ; ipert1 < _natom ; ++ipert1 ) {
      for ( unsigned idir1 = 0 ; idir1 < 3 ; ++idir1 ) {
        Eigen::Vector3cd d2red;
        d2red[0] = _d2cart(ipert2*3  ,ipert1*3+idir1);
        d2red[1] = _d2cart(ipert2*3+1,ipert1*3+idir1); 
        d2red[2] = _d2cart(ipert2*3+2,ipert1*3+idir1);
        const Eigen::Vector3cd d2cart(_gprim * d2red);
        _d2cart(ipert2*3  ,ipert1*3+idir1) = d2cart[0];
        _d2cart(ipert2*3+1,ipert1*3+idir1) = d2cart[1];
        _d2cart(ipert2*3+2,ipert1*3+idir1) = d2cart[2];
      }
    }
  }

  if ( _hasASR ) this->applyASR();
#else
  throw EXCEPTION("Functionnality not available without EIGEN support !!",ERRDIV);
#endif

}


void PhononMode::computeASR(const Ddb& ddb) {
  std::clog << "-- Computing Acousitc Sum Rule correction...";

  this->resize(ddb.natom());

#ifdef HAVE_EIGEN
  PhononMode phononGamma;

  try {
    phononGamma.computeForceCst({{0.0e0,0.0e0,0.0e0}}, ddb);
  }
  catch (Exception& e) {
    e.ADD("Can not calculate dynamical matrix for gamma q-pt -> Abording.",ERRDIV);
    throw e;
  }

  _asr.resize(3*_natom,3);
  _asr.fill(complex(0.0e0,0.0e0));
  for ( unsigned ipert1 = 0 ; ipert1 < _natom ; ++ipert1 ) {
    const unsigned iblock1 = ipert1*3;
    for ( unsigned ipert2 = 0 ; ipert2 < _natom ; ++ipert2 ) {
      const unsigned iblock2 = ipert2*3;
      //_asr.block(iblock1,0,3,3) += phonongamma._d2cart.block(iblock1,iblock2, 3, 3);
      _asr(iblock1  ,0) += phononGamma._d2cart(iblock1  ,iblock2  );
      _asr(iblock1  ,1) += phononGamma._d2cart(iblock1  ,iblock2+1);
      _asr(iblock1  ,2) += phononGamma._d2cart(iblock1  ,iblock2+2);
      _asr(iblock1+1,0) += phononGamma._d2cart(iblock1+1,iblock2+0);
      _asr(iblock1+1,1) += phononGamma._d2cart(iblock1+1,iblock2+1);
      _asr(iblock1+1,2) += phononGamma._d2cart(iblock1+1,iblock2+2);
      _asr(iblock1+2,0) += phononGamma._d2cart(iblock1+2,iblock2+0);
      _asr(iblock1+2,1) += phononGamma._d2cart(iblock1+2,iblock2+1);
      _asr(iblock1+2,2) += phononGamma._d2cart(iblock1+2,iblock2+2);
    }
  }
  //std::clog << std::endl << _asr << std::endl;
  std::clog << "Done --" << std::endl;
  _hasASR = true;
#else
  throw EXCEPTION("Functionnality not available without EIGEN support !!",ERRDIV);
#endif
}

void PhononMode::applyASR() {
#ifdef HAVE_EIGEN
  std::clog << "-- Applying Acoustic Sum Rule correction...";

  if ( !_hasASR )
    throw EXCEPTION("ASR Can not been applied since it has not been calculated",ERRDIV);

  for ( unsigned ipert1 = 0 ; ipert1 < _natom ; ++ipert1 ) {
    const unsigned iblock1 = ipert1*3;
    //_d2cart.block(iblock1,iblock1, 3, 3) -= _asr.block(iblock1,0,3,3) 
    _d2cart(iblock1  ,iblock1  ) -= _asr(iblock1  ,0);
    _d2cart(iblock1  ,iblock1+1) -= _asr(iblock1  ,1);
    _d2cart(iblock1  ,iblock1+2) -= _asr(iblock1  ,2);
    _d2cart(iblock1+1,iblock1+0) -= _asr(iblock1+1,0);
    _d2cart(iblock1+1,iblock1+1) -= _asr(iblock1+1,1);
    _d2cart(iblock1+1,iblock1+2) -= _asr(iblock1+1,2);
    _d2cart(iblock1+2,iblock1+0) -= _asr(iblock1+2,0);
    _d2cart(iblock1+2,iblock1+1) -= _asr(iblock1+2,1);
    _d2cart(iblock1+2,iblock1+2) -= _asr(iblock1+2,2);
  }
  std::clog << "Done --" << std::endl;
#else
  throw EXCEPTION("Functionnality not available without EIGEN support !!",ERRDIV);
#endif
}


//
void PhononMode::computeEigen(double *freq, complex *mode ) {

#ifdef HAVE_EIGEN
  // Build dynamical matrix
  Eigen::MatrixXcd dynMat(3*_natom,3*_natom);
  for ( unsigned iblock1 = 0 ; iblock1 < _natom ; ++iblock1 ) {
    const double mass1 = _mass[iblock1];
    for ( unsigned iblock2= 0 ; iblock2 < _natom ; ++iblock2 ) {
      const double invsqrtM1M2 = 1.0e0/(sqrt(mass1*_mass[iblock2]));
      dynMat(iblock1*3  , iblock2*3  ) = _d2cart(iblock1*3  ,iblock2*3  )*invsqrtM1M2;
      dynMat(iblock1*3  , iblock2*3+1) = _d2cart(iblock1*3  ,iblock2*3+1)*invsqrtM1M2;
      dynMat(iblock1*3  , iblock2*3+2) = _d2cart(iblock1*3  ,iblock2*3+2)*invsqrtM1M2;
    }
    for ( unsigned iblock2= 0 ; iblock2 < _natom ; ++iblock2 ) {
      const double invsqrtM1M2 = 1.0e0/(sqrt(mass1*_mass[iblock2]));
      dynMat(iblock1*3+1, iblock2*3  ) = _d2cart(iblock1*3+1,iblock2*3  )*invsqrtM1M2;
      dynMat(iblock1*3+1, iblock2*3+1) = _d2cart(iblock1*3+1,iblock2*3+1)*invsqrtM1M2;
      dynMat(iblock1*3+1, iblock2*3+2) = _d2cart(iblock1*3+1,iblock2*3+2)*invsqrtM1M2;
    }
    for ( unsigned iblock2= 0 ; iblock2 < _natom ; ++iblock2 ) {
      const double invsqrtM1M2 = 1.0e0/(sqrt(mass1*_mass[iblock2]));
      dynMat(iblock1*3+2, iblock2*3  ) = _d2cart(iblock1*3+2,iblock2*3  )*invsqrtM1M2;
      dynMat(iblock1*3+2, iblock2*3+1) = _d2cart(iblock1*3+2,iblock2*3+1)*invsqrtM1M2;
      dynMat(iblock1*3+2, iblock2*3+2) = _d2cart(iblock1*3+2,iblock2*3+2)*invsqrtM1M2;

    }
  }
  //Make it hermitian/SelfAdjoint
  dynMat = (dynMat + dynMat.adjoint() )*0.5e0;

  Eigen::SelfAdjointEigenSolver<Eigen::MatrixXcd> solver(dynMat);
  auto eigenvalues = solver.eigenvalues();
  _eigenVec = solver.eigenvectors();
  /*
  std::cout.setf(std::ios::fixed, std::ios::floatfield);
  std::cout.setf(std::ios::right, std::ios::adjustfield);
  std::cout.precision(5);
  std::cout << "# For Q-pt " << std::setw(9) << _qpt[0] << std::setw(9) << _qpt[1]  << std::setw(9) << _qpt [2]
    <<" frequencies are [Ha]:" << std::endl;;
  std::cout.setf(std::ios::scientific, std::ios::floatfield);
  std::cout.precision(6);
  */
  for ( unsigned imode = 0 ; imode < _natom ; ++imode ) {
    const double freq0 = eigenvalues(imode*3  );
    const double freq1 = eigenvalues(imode*3+1);
    const double freq2 = eigenvalues(imode*3+2);
    _frequencies[imode*3  ] = ( freq0 < 0.0e0 ) ? -std::sqrt(-freq0) : std::sqrt(freq0);
    _frequencies[imode*3+1] = ( freq1 < 0.0e0 ) ? -std::sqrt(-freq1) : std::sqrt(freq1);
    _frequencies[imode*3+2] = ( freq2 < 0.0e0 ) ? -std::sqrt(-freq2) : std::sqrt(freq2);
    /*
    std::cout << std::setw(14) << _frequencies[imode*3  ]
      << std::setw(14) << _frequencies[imode*3+1]
      << std::setw(14) << _frequencies[imode*3+2] << std::endl;
      */
  }
  
  for ( unsigned imode = 0 ; imode < 3*_natom ; ++imode ) {
    auto mode = _eigenVec.col(imode).normalized();
    for ( unsigned iatom = 0 ; iatom < 3*_natom ; ++iatom ) {
      const double factor = 1.e0/sqrt(_mass[iatom/3]);
      _eigenDisp(imode,iatom) = mode(iatom)*factor;
    }
  }

  // Force all coeff smaller than 1e-7 to be 0 (mimic anaddb)
  for ( unsigned imode = 0 ; imode < 3*_natom ; ++imode ) {
    for ( unsigned iatom = 0 ; iatom < 3*_natom ; ++iatom ) {
      using std::abs;
      auto tmp = _eigenDisp(imode,iatom);
      _eigenDisp(imode,iatom) = complex((abs(tmp.real()) > 1e-7 ? tmp.real() : 0), (abs(tmp.imag()) > 1e-7 ? tmp.imag() : 0));
    }
  }

  if ( freq != nullptr ) {
    //copy frequencies
    Eigen::Map<Eigen::VectorXd> mfreq(freq,3*_natom);
    mfreq = _frequencies;
  }
  if ( mode != nullptr ) {
    //copy eigendisp
    // Be carefull, each column is a mode but Matrix are colWise by default whereas the modes are "rowwise" mode by mode |mode 1      |mode 2      | 
    Eigen::Map<Eigen::Matrix<complex,Eigen::Dynamic,Eigen::Dynamic,Eigen::RowMajor>> mdisp(mode,3*_natom,3*_natom);
    mdisp = _eigenDisp;
  }

  /*
  std::cout << "# Eigen displacements are (columnwise) [bohr]:" << std::endl;
  for ( unsigned iatom = 0 ; iatom < 3*_natom ; ++iatom ) {
    for ( unsigned imode = 0 ; imode < 3*_natom ; ++imode ) {
      std::cout << std::setw(15) << _eigenDisp(imode,iatom).real();
    }
    std::cout << std::endl;
  }
  */
#else
  throw EXCEPTION("Functionnality not available without EIGEN support !!",ERRDIV);
  (void) freq;
  (void) mode;
#endif
}


//
void PhononMode::computeAllEigen(const Ddb& ddb, double *freq, complex *modes) {
  _natom = ddb.natom();
  // resize matrix
#ifdef HAVE_EIGEN
  _d2cart.resize(3*_natom,3*_natom);
  _eigenVec.resize(3*_natom,3*_natom);
  _eigenDisp.resize(3*_natom,3*_natom);
  _frequencies.resize(3*_natom);
  _mass.resize(_natom);
  const geometry::mat3d gprim = ddb.gprim();
  _gprim << gprim[0], gprim[1],gprim[2],
         gprim[3], gprim[4], gprim[5],
         gprim[6], gprim[7], gprim[8];

  bool isNormalized = ddb.isNormalized();
  for ( unsigned iatom = 0 ; iatom < _natom ; ++iatom ) {
    _mass[iatom] = (isNormalized?1.:MendeTable.mass[ddb.znucl().at(ddb.typat().at(iatom)-1)]*phys::amu_emass); // type starts at 1
  }

  unsigned iqpt = 0;
  for ( auto& qpt : ddb.getQpts() ) {
    // Initialize qpt
    _qpt << qpt[0], qpt[1], qpt[2];

    try {
      this->computeForceCst(ddb.getDdb(qpt));
      double *qfreq = nullptr;
      complex *qmode = nullptr;
      if ( freq != nullptr ) {
        qfreq = &freq[iqpt*3*_natom];
        if ( modes != nullptr )
          qmode = &modes[iqpt*3*_natom*3*_natom];
      }
      this->computeEigen(qfreq,qmode);
    }
    catch (Exception& e) {
      e.ADD("Abording",ERRDIV);
      throw e;
    }
    ++iqpt;
  }
#else
  throw EXCEPTION("Functionnality not available without EIGEN support !!",ERRDIV);
  (void) freq;
  (void) modes;
#endif
}
