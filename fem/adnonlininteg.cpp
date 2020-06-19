// Copyright (c) 2010, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-443211. All Rights
// reserved. See file COPYRIGHT for details.
//
// This file is part of the MFEM library. For more information and source code
// availability see http://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the GNU Lesser General Public License (as published by the Free
// Software Foundation) version 2.1 dated February 1999.

#include "fem.hpp"
#include "../general/forall.hpp"
#include "adnonlininteg.hpp"

namespace mfem
{


void ADQIntegratorJ::QIntegratorDD(const Vector &vparam, const Vector &uu, DenseMatrix &jac)
{
#ifdef MFEM_USE_ADEPT
    //use ADEPT package
    adept::Stack* p_stack=adept::active_stack();
    p_stack->deactivate();

    int n=uu.Size();
    jac.SetSize(m,n);
    jac=0.0;
    m_stack.activate();
    {
        ADFVector aduu(uu);
        ADFVector rr(m); //residual vector
        m_stack.new_recording();
        this->QIntegratorDU(vparam,aduu,rr);
        m_stack.independent(aduu.GetData(), n);//independent variables
        m_stack.dependent(rr.GetData(), m);//dependent variables
        m_stack.jacobian(jac.Data());
    }
    m_stack.deactivate();
#elif MFEM_USE_CODIPACK

#else
    //use native AD package
   int n=uu.Size();
   jac.SetSize(m,n);
   jac=0.0;
   {
       ADFVector aduu(uu); //all dual numbers are initialized to zero
       ADFVector rr;

       for(int ii=0;ii<n;ii++){
           aduu[ii].dual(1.0);
           this->QIntegratorDU(vparam,aduu,rr);
           for(int jj=0;jj<m;jj++)
           {
               jac(jj,ii)=rr[jj].dual();
           }
           aduu[ii].dual(0.0);
       }
   }
#endif
}

void ADQIntegratorH::QIntegratorDU(const Vector &vparam, Vector &uu, Vector &rr)
{
    int n=uu.Size();
    rr.SetSize(n);
    ADFVector aduu(uu);
    ADFType   rez;
    for(int ii=0;ii<n;ii++)
    {
        aduu[ii].dual(1.0);
        rez=this->QIntegrator(vparam,aduu);
        rr[ii]=rez.dual();
        aduu[ii].dual(0.0);
    }
}

void ADQIntegratorH::QIntegratorDD(const Vector &vparam, const Vector &uu, DenseMatrix &jac)
{
    int n=uu.Size();
    jac.SetSize(n);
    jac=0.0;
    {
        ADSVector aduu(n);
        for(int ii = 0;  ii < n ; ii++)
        {
            aduu[ii].real(ADFType(uu[ii],0.0));
            aduu[ii].dual(ADFType(0.0,0.0));
        }

        for(int ii = 0; ii < n ; ii++)
        {
            aduu[ii].dual(ADFType(uu[ii],1.0));
            for(int jj=0; jj<(ii+1); jj++)
            {
                aduu[jj].dual(ADFType(1.0,0.0));
                ADSType rez= this->QIntegrator(vparam,aduu);
                jac(ii,jj)=rez.dual().dual();
                jac(jj,ii)=rez.dual().dual();
                aduu[jj].dual(ADFType(0.0,0.0));
            }
            aduu[ii].dual(ADFType(uu[ii],0.0));
        }
    }
}





double ADNonlinearFormIntegratorH::GetElementEnergy(const mfem::FiniteElement & el,
                                    mfem::ElementTransformation & Tr, 
                                    const mfem::Vector & elfun)
{
    return this->ElementEnergy(el,Tr,elfun);
}

void ADNonlinearFormIntegratorH::AssembleElementVector(const mfem::FiniteElement & el,
                                            mfem::ElementTransformation & Tr, 
                                            const mfem::Vector & elfun, mfem::Vector & elvect)
{
    
    int ndof = el.GetDof();
    elvect.SetSize(ndof);
    
    {
        ADFVector adelfun(elfun);
        //all dual numbers in adelfun are initialized to 0.0
        for(int ii = 0; ii < adelfun.Size(); ii++)
        {
            //set the dual for the ii^th element to 1.0
            adelfun[ii].dual(1.0);
            ADFType rez= this->ElementEnergy(el,Tr, adelfun);
            elvect[ii]=rez.dual();
            //return it back to zero
            adelfun[ii].dual(0.0);
        }
    }
    
}

void ADNonlinearFormIntegratorH::AssembleElementGrad(const mfem::FiniteElement & el,
                                          mfem::ElementTransformation & Tr, 
                                          const mfem::Vector & elfun, 
                                          mfem::DenseMatrix & elmat)
{
    
    int ndof = el.GetDof();
    elmat.SetSize(ndof);
    elmat=0.0;
    {
        ADSVector adelfun(ndof);
        for(int ii = 0; ii < ndof; ii++)
        {
            adelfun[ii].real(ADFType(elfun[ii],0.0));
            adelfun[ii].dual(ADFType(0.0,0.0));
        }
        
        for(int ii = 0; ii < adelfun.Size(); ii++)
        {
            adelfun[ii].real(ADFType(elfun[ii],1.0));
            for(int jj = 0; jj < (ii+1); jj++)
            {
                adelfun[jj].dual(ADFType(1.0,0.0));
                ADSType rez= this->ElementEnergy(el,Tr, adelfun);
                elmat(ii,jj)=rez.dual().dual();
                elmat(jj,ii)=rez.dual().dual();
                adelfun[jj].dual(ADFType(0.0,0.0));
            }
            adelfun[ii].real(ADFType(elfun[ii],0.0));
        }
        
    }
    
}

    
} //end namespace mfem
