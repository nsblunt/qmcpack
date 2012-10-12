/////////////////////////////////////////////////////////////////
// (c) Copyright 2003-  by Ken Esler and Jeongnim Kim           //
//////////////////////////////////////////////////////////////////
//   National Center for Supercomputing Applications &          //
//   Materials Computation Center                               //
//   University of Illinois, Urbana-Champaign                   //
//   Urbana, IL 61801                                           //
//   e-mail: jnkim@ncsa.uiuc.edu                                //
//                                                              //
// Supported by                                                 //
//   National Center for Supercomputing Applications, UIUC      //
//   Materials Computation Center, UIUC                         //
//////////////////////////////////////////////////////////////////

#ifndef QMCPLUSPLUS_EINSPLINE_BUILDER_ESHDF_H
#define QMCPLUSPLUS_EINSPLINE_BUILDER_ESHDF_H
#include "Utilities/ProgressReportEngine.h"
#include "Message/CommOperators.h"
#include <QMCWaveFunctions/einspline_helper.hpp>

namespace qmcplusplus {

  template<typename SPE>
    void copy(EinsplineSet* in, SPE* out)
    {
      out->PrimLattice=in->PrimLattice;
      out->SuperLattice=in->SuperLattice;
      out->GGt=in->GGt;
      out->setOrbitalSetSize(in->getOrbitalSetSize());
    }

  template<typename SPE>
  void EinsplineSetBuilder::ReadBands_ESHDF_Complex(int spin, SPE* orbitalSet)
  {

    ReportEngine PRE("EinsplineSetBuilder","ReadBands_ESHDF_Complex(spin,SPE*)");

    typedef typename SPE::value_type spline_data_type;

    Timer c_prep, c_unpack,c_fft, c_phase, c_spline, c_newphase, c_h5, c_init;
    double t_prep=0.0, t_unpack=0.0, t_fft=0.0, t_phase=0.0, t_spline=0.0, t_newphase=0.0, t_h5=0.0, t_init=0.0;

    orbitalSet->PrimLattice=Lattice;

    c_prep.restart();
    bool root = myComm->rank()==0;
    // bcast other stuff
    myComm->bcast (NumDistinctOrbitals);
    myComm->bcast (NumValenceOrbs);
    myComm->bcast (NumCoreOrbs);
    int N = NumDistinctOrbitals;

    cout << "DEBUG distinct " << N << " valence " << NumValenceOrbs << endl;
    orbitalSet->resizeStorage(N,NumValenceOrbs);

    //orbitalSet->kPoints.resize(N);
    //orbitalSet->MakeTwoCopies.resize(N);
    //orbitalSet->StorageValueVector.resize(N); orbitalSet->BlendValueVector.resize(N);
    //orbitalSet->StorageLaplVector.resize(N);  orbitalSet->BlendLaplVector.resize(N);
    //orbitalSet->StorageGradVector.resize(N);  orbitalSet->BlendGradVector.resize(N);
    //orbitalSet->StorageHessVector.resize(N);
    //orbitalSet->StorageGradHessVector.resize(N);
    //orbitalSet->phase.resize(N);
    //orbitalSet->eikr.resize(N);
    //orbitalSet->NumValenceOrbs = NumValenceOrbs;
    //orbitalSet->NumCoreOrbs    = NumCoreOrbs;

    // Read in k-points
    int numOrbs = orbitalSet->getOrbitalSetSize();
    int num = 0;
    if (root) {
      for (int iorb=0; iorb<N; iorb++) {
	int ti = SortBands[iorb].TwistIndex;
	PosType twist  = TwistAngles[ti];
	orbitalSet->kPoints[iorb] = orbitalSet->PrimLattice.k_cart(twist);
	orbitalSet->MakeTwoCopies[iorb] = 
	  (num < (numOrbs-1)) && SortBands[iorb].MakeTwoCopies;
	num += orbitalSet->MakeTwoCopies[iorb] ? 2 : 1;
      }
    }
    myComm->bcast(orbitalSet->kPoints);
    myComm->bcast(orbitalSet->MakeTwoCopies);

    if(!orbitalSet->isready()) APP_ABORT("EinsplineSetBuilder::ReadBands_ESHDF_Complex");

    // First, check to see if we have already read this in
    H5OrbSet set(H5FileName, spin, N);
    
    ///check mesh or ready for FFT grid
    bool havePsig=ReadGvectors_ESHDF();
    app_log() << "MeshSize = (" << MeshSize[0] << ", " << MeshSize[1] << ", " << MeshSize[2] << ")\n";

    int nx, ny, nz, bi, ti;
    nx=MeshSize[0]; ny=MeshSize[1]; nz=MeshSize[2];

    Ugrid xyz_grid[3];
    typename SPE::BCType xyz_bc[3];
    xyz_grid[0].start = 0.0;  xyz_grid[0].end = 1.0;  xyz_grid[0].num = nx;
    xyz_grid[1].start = 0.0;  xyz_grid[1].end = 1.0;  xyz_grid[1].num = ny;
    xyz_grid[2].start = 0.0;  xyz_grid[2].end = 1.0;  xyz_grid[2].num = nz;

    xyz_bc[0].lCode=PERIODIC; xyz_bc[0].rCode=PERIODIC;
    xyz_bc[1].lCode=PERIODIC; xyz_bc[1].rCode=PERIODIC;
    xyz_bc[2].lCode=PERIODIC; xyz_bc[2].rCode=PERIODIC;

    orbitalSet->allocate(xyz_grid,xyz_bc,NumValenceOrbs);
    
    int isComplex=1;
    if (root) 
    {
      HDFAttribIO<int> h_isComplex(isComplex);
      h_isComplex.read(H5FileID, "/electrons/psi_r_is_complex");
    }
    myComm->bcast(isComplex);
    if (!isComplex) 
    {
      APP_ABORT("Expected complex orbitals in ES-HDF file, but found real ones.");
    }

    //EinsplineSetBuilder::RotateBands_ESHDF(spin, orbitalSet);

    t_prep += c_prep.elapsed();

    /** For valence orbitals,
     * - extended orbitals either in G or in R
     * - localized orbitals
     */
    Array<spline_data_type,3> splineData(nx,ny,nz);

    if(havePsig)//perform FFT using FFTW
    {
      c_init.restart();
      Array<complex<double>,3> FFTbox;
      FFTbox.resize(MeshSize[0], MeshSize[1], MeshSize[2]);
      fftw_plan FFTplan = fftw_plan_dft_3d 
        (MeshSize[0], MeshSize[1], MeshSize[2],
         reinterpret_cast<fftw_complex*>(FFTbox.data()),
         reinterpret_cast<fftw_complex*>(FFTbox.data()),
         +1, FFTW_ESTIMATE);

      Vector<complex<double> > cG(MaxNumGvecs);

      //this will be parallelized with OpenMP
      for(int iorb=0,ival=0; iorb<N; ++iorb, ++ival)
      {
        //Vector<complex<double> > cG;
        int ncg=0;
        int ti=SortBands[iorb].TwistIndex;
        c_h5.restart();

        if(root)
        {
          ostringstream path;
          path << "/electrons/kpoint_" << ti    //SortBands[iorb].TwistIndex 
            << "/spin_" << spin << "/state_" << SortBands[iorb].BandIndex << "/psi_g";
          HDFAttribIO<Vector<complex<double> > >  h_cG(cG);
          h_cG.read (H5FileID, path.str().c_str());
          ncg=cG.size();
        }
        myComm->bcast(ncg);
        if(ncg != Gvecs[ti].size())
        {
          APP_ABORT("Failed : ncg != Gvecs[ti].size()");
        }

        if(!root) cG.resize(ncg);
        myComm->bcast(cG);

        t_h5 += c_h5.elapsed();

        c_unpack.restart();
        unpack4fftw(cG,Gvecs[ti],MeshSize,FFTbox);
        t_unpack+= c_unpack.elapsed();

        c_fft.restart();
        fftw_execute (FFTplan);
        t_fft+= c_fft.elapsed();

        c_phase.restart();
        fix_phase_rotate_c2c(FFTbox,splineData,TwistAngles[ti]);
        t_phase+= c_phase.elapsed();

        c_spline.restart();
        einspline::set(orbitalSet->MultiSpline, ival, splineData.data());
        t_spline+= c_spline.elapsed();
      }

      fftw_destroy_plan(FFTplan);
      t_init+=c_init.elapsed();
    }
    else
    {
      Array<complex<double>,3> rawData(nx,ny,nz);
      //this will be parallelized with OpenMP
      for(int iorb=0,ival=0; iorb<N; ++iorb, ++ival)
      {
        //check dimension
        if(root)
        {
          ostringstream path;
          path << "/electrons/kpoint_" << SortBands[iorb].TwistIndex 
            << "/spin_" << spin << "/state_" << SortBands[iorb].BandIndex << "/psi_r";
          HDFAttribIO<Array<complex<double>,3> >  h_splineData(rawData);
          h_splineData.read(H5FileID, path.str().c_str());
          simd::copy(splineData.data(),rawData.data(),rawData.size());
        }
        myComm->bcast(splineData);
        einspline::set(orbitalSet->MultiSpline, ival, splineData.data());
      }
    }

    app_log() << "    READBANDS::PREP   = " << t_prep << endl;
    app_log() << "    READBANDS::H5     = " << t_h5 << endl;
    app_log() << "    READBANDS::UNPACK = " << t_unpack << endl;
    app_log() << "    READBANDS::FFT    = " << t_fft << endl;
    app_log() << "    READBANDS::PHASE  = " << t_phase << endl;
    app_log() << "    READBANDS::SPLINE = " << t_spline << endl;
    app_log() << "    READBANDS::SUM    = " << t_init << endl;


    //ExtendedMap_z[set] = orbitalSet->MultiSpline;
  }



  template<typename SPE>
  void EinsplineSetBuilder::ReadBands_ESHDF_Real(int spin, SPE* orbitalSet)
  {
    ReportEngine PRE("EinsplineSetBuilder","ReadBands_ESHDF_Real(spin,SPE*,need2convert)");

    bool root = myComm->rank()==0;
    // bcast other stuff
    myComm->bcast (NumDistinctOrbitals);
    myComm->bcast (NumValenceOrbs);
    myComm->bcast (NumCoreOrbs);
    int N = NumDistinctOrbitals;

    typedef typename SPE::DataType spline_data_type;
    //typedef double spline_data_type;
    orbitalSet->PrimLattice=Lattice;

    orbitalSet->resizeStorage(N,NumValenceOrbs);

    //orbitalSet->kPoints.resize(N);
    //orbitalSet->MakeTwoCopies.resize(N);
    //orbitalSet->StorageValueVector.resize(N); orbitalSet->BlendValueVector.resize(N);
    //orbitalSet->StorageLaplVector.resize(N);  orbitalSet->BlendLaplVector.resize(N);
    //orbitalSet->StorageGradVector.resize(N);  orbitalSet->BlendGradVector.resize(N);
    //orbitalSet->StorageHessVector.resize(N);
    //orbitalSet->StorageGradHessVector.resize(N);
    //orbitalSet->phase.resize(N);
    //orbitalSet->eikr.resize(N);
    //orbitalSet->NumValenceOrbs = NumValenceOrbs;
    //orbitalSet->NumCoreOrbs    = NumCoreOrbs;
    //orbitalSet->FirstOrderSplines.resize(IonPos.size());
    // Read in k-points
    
    int numOrbs = orbitalSet->getOrbitalSetSize();
    int num = 0;
    if (root) {
      for (int iorb=0; iorb<N; iorb++) {
	int ti = SortBands[iorb].TwistIndex;
	PosType twist  = TwistAngles[ti];
	orbitalSet->kPoints[iorb] = orbitalSet->PrimLattice.k_cart(twist);
	orbitalSet->MakeTwoCopies[iorb] = 
	  (num < (numOrbs-1)) && SortBands[iorb].MakeTwoCopies;
	num += orbitalSet->MakeTwoCopies[iorb] ? 2 : 1;
      }
      PosType twist0 = TwistAngles[SortBands[0].TwistIndex];
      for (int i=0; i<OHMMS_DIM; i++)
	if (std::fabs(std::fabs(twist0[i]) - 0.5) < 1.0e-8)
	  orbitalSet->HalfG[i] = 1;
	else
	  orbitalSet->HalfG[i] = 0;
      //EinsplineSetBuilder::RotateBands_ESHDF(spin, orbitalSet);
    }
    myComm->bcast(orbitalSet->kPoints);
    myComm->bcast(orbitalSet->MakeTwoCopies);
    myComm->bcast(orbitalSet->HalfG);

    // First, check to see if we have already read this in
    H5OrbSet set(H5FileName, spin, N);
    
    bool havePsir=!ReadGvectors_ESHDF();

    app_log() << "HalfG = " << orbitalSet->HalfG << endl;
    app_log() << "MeshSize = (" << MeshSize[0] << ", " 
	      << MeshSize[1] << ", " << MeshSize[2] << ")\n";

    //int nx, ny, nz, bi, ti;
    int nx, ny, nz;
    nx=MeshSize[0]; ny=MeshSize[1]; nz=MeshSize[2];

    Ugrid xyz_grid[3];
    typename SPE::BCType xyz_bc[3];
    xyz_grid[0].start = 0.0;  xyz_grid[0].end = 1.0;  xyz_grid[0].num = nx;
    xyz_grid[1].start = 0.0;  xyz_grid[1].end = 1.0;  xyz_grid[1].num = ny;
    xyz_grid[2].start = 0.0;  xyz_grid[2].end = 1.0;  xyz_grid[2].num = nz;

    for(int i=0; i<3; ++i)
      xyz_bc[i].lCode=xyz_bc[i].rCode=(orbitalSet->HalfG[i])? ANTIPERIODIC:PERIODIC;
    orbitalSet->allocate(xyz_grid,xyz_bc,NumValenceOrbs);

    //if (HaveOrbDerivs) {
    //  orbitalSet->FirstOrderSplines.resize(IonPos.size());
    //  for (int ion=0; ion<IonPos.size(); ion++)
    //    for (int dir=0; dir<OHMMS_DIM; dir++)
    //      orbitalSet->FirstOrderSplines[ion][dir] = 
    //        create_multi_UBspline_3d_d (x_grid, y_grid, z_grid, xBC, yBC, zBC, NumValenceOrbs);
    //}
           
    int isComplex;
    if (root) {
      HDFAttribIO<int> h_isComplex(isComplex);
      h_isComplex.read(H5FileID, "/electrons/psi_r_is_complex");
    }
    myComm->bcast(isComplex);

    bool isCore = bcastSortBands(N,root);
    if(isCore)
    {
      APP_ABORT("Core states not supported by ES-HDF yet.");
    }

    //this is common
    if(havePsir)
    {
      if(isComplex)
      {
        app_log() << "   Reading complex psi_r and convert to real" << endl;
        Array<spline_data_type,3> splineData(nx,ny,nz);
        Array<complex<double>,3> rawData;
        for(int iorb=0,ival=0; iorb<N; ++iorb, ++ival)
        {
          int ti=SortBands[iorb].TwistIndex;
          if(root)
          {
            ostringstream path;
            path << "/electrons/kpoint_" << SortBands[iorb].TwistIndex 
              << "/spin_" << spin << "/state_" << SortBands[iorb].BandIndex << "/psi_r";
            HDFAttribIO<Array<complex<double>,3> >  h_splineData(rawData);
            h_splineData.read(H5FileID, path.str().c_str());
          }
          myComm->bcast(rawData);
          //multiply twist factor and project on the real
          fix_phase_c2r(rawData,splineData,TwistAngles[ti]);
          einspline::set(orbitalSet->MultiSpline, ival,splineData.data());
          //set_multi_UBspline_3d_d (orbitalSet->MultiSpline, ival, splineData.data());
        }
      }
      else
      {
        app_log() << "   Reading real psi_r" << endl;
        Array<spline_data_type,3> splineData(nx,ny,nz);
        Array<double,3> rawData(nx,ny,nz);
        for(int iorb=0,ival=0; iorb<N; ++iorb, ++ival)
        {
          if(root)
          {
            ostringstream path;
            path << "/electrons/kpoint_" << SortBands[iorb].TwistIndex 
              << "/spin_" << spin << "/state_" << SortBands[iorb].BandIndex << "/psi_r";
            HDFAttribIO<Array<double,3> >  h_splineData(rawData);
            h_splineData.read(H5FileID, path.str().c_str());
            simd::copy(splineData.data(),rawData.data(),rawData.size());
          }
          myComm->bcast(splineData);
          einspline::set(orbitalSet->MultiSpline, ival,splineData.data());
        }
      }
    }
    else
    {
      Array<ComplexType,3> FFTbox;
      FFTbox.resize(MeshSize[0], MeshSize[1], MeshSize[2]);
      fftw_plan FFTplan = fftw_plan_dft_3d 
        (MeshSize[0], MeshSize[1], MeshSize[2],
         reinterpret_cast<fftw_complex*>(FFTbox.data()),
         reinterpret_cast<fftw_complex*>(FFTbox.data()),
         +1, FFTW_ESTIMATE);

      Array<spline_data_type,3> splineData(nx,ny,nz);

      for(int iorb=0,ival=0; iorb<N; ++iorb, ++ival)
      {
        Vector<complex<double> > cG;
        int ncg=0;
        int ti=SortBands[iorb].TwistIndex;
        if(root)
        {
          ostringstream path;
          path << "/electrons/kpoint_" << ti    //SortBands[iorb].TwistIndex 
            << "/spin_" << spin << "/state_" << SortBands[iorb].BandIndex << "/psi_g";
          cout << "FFT on " <<  path.str() << endl;
          HDFAttribIO<Vector<complex<double> > >  h_cG(cG);
          h_cG.read (H5FileID, path.str().c_str());
          ncg=cG.size();
        }
        myComm->bcast(ncg);
        if(ncg != Gvecs[ti].size())
        {
          APP_ABORT("Failed : ncg != Gvecs[ti].size()");
        }

        if(!root) cG.resize(ncg);
        myComm->bcast(cG);

        unpack4fftw(cG,Gvecs[ti],MeshSize,FFTbox);
        fftw_execute (FFTplan);
        fix_phase_rotate_c2r(FFTbox,splineData,TwistAngles[ti]);
        einspline::set(orbitalSet->MultiSpline, ival,splineData.data());
      }

      fftw_destroy_plan(FFTplan);
    }

    //ExtendedMap_d[set] = orbitalSet->MultiSpline;
  }

}
#endif

/***************************************************************************
 * $RCSfile$   $Author: jeongnim.kim $
 * $Revision: 5260 $   $Date: 2011-06-18 07:45:58 -0400 (Sat, 18 Jun 2011) $
 * $Id: EinsplineSetBuilderESHDF.cpp 5260 2011-06-18 11:45:58Z jeongnim.kim $
 ***************************************************************************/
