#ifndef __MFEM_ELIMINATION_HPP
#define __MFEM_ELIMINATION_HPP

#include "mfem.hpp"

using namespace mfem;

/// convenience function, convert a SparseMatrix to a (serial)
/// HypreParMatrix so you can use hypre solvers
mfem::HypreParMatrix* SerialHypreMatrix(mfem::SparseMatrix& mat, bool transfer_ownership=true)
{
   HYPRE_Int row_starts[3];
   row_starts[0] = 0;
   row_starts[1] = mat.Height();
   row_starts[2] = mat.Height();
   HYPRE_Int col_starts[3];
   col_starts[0] = 0;
   col_starts[1] = mat.Width();
   col_starts[2] = mat.Width();
   mfem::HypreParMatrix * out = new mfem::HypreParMatrix(
      MPI_COMM_WORLD, mat.Height(), mat.Width(),
      row_starts, col_starts, &mat);
   out->CopyRowStarts();
   out->CopyColStarts();

   /// 3 gives MFEM full ownership of i, j, data
   if (transfer_ownership)
   {
      out->SetOwnerFlags(3, out->OwnsOffd(), out->OwnsColMap());
      mat.LoseData();
   }
   else
   {
      out->SetOwnerFlags(0, out->OwnsOffd(), out->OwnsColMap());
   }

   return out;
}

/**
   Take a vector with displacements and lagrange multiplier degrees of freedom
   (corresponding to pressures on the slave surface), eliminate the
   constraint, return vector of just displacements.

   This is P in the EliminationSolver algorithm

   Height is number of total displacements, Width is smaller, with some
   displacements eliminated via constraints.
*/
class EliminationProjection : public Operator
{
public:
   /**
      Lots to think about in this interface, but I think a cleaner version
      takes just the jac. Actually, what I want is an object that creates
      both this and the approximate version, using only jac.

      rectangular C_1 = C_m has lagrange_dofs rows, master_contact_dofs columns
      square C_2 = C_s has lagrange_dofs rows, slave_contact_dofs columns

      C_m maps master displacements into lagrange space
      C_s maps slave displacements into lagrange space
      C_s^T maps lagrange space to slave displacements (*)
      C_s^{-1} maps lagrange space into slave displacements
      -C_s^{-1} C_m maps master displacements to slave displacements
   */
   EliminationProjection(SparseMatrix& A, SparseMatrix& B,
                         mfem::Array<int>& master_contact_dofs,
                         mfem::Array<int>& slave_contact_dofs);

   void Mult(const mfem::Vector& in, mfem::Vector& out) const;
   void MultTranspose(const mfem::Vector& in, mfem::Vector& out) const;

   mfem::SparseMatrix * AssembleApproximate() const;

   void BuildGTilde(const mfem::Vector& g, mfem::Vector& gtilde) const;

   void RecoverPressure(const mfem::Vector& disprhs,
                        const mfem::Vector& disp, mfem::Vector& pressure) const;

private:
   SparseMatrix& A_;
   SparseMatrix& B_;

   mfem::Array<int>& master_contact_dofs_;
   mfem::Array<int>& slave_contact_dofs_;

   mfem::DenseMatrix Cm_;
   mfem::DenseMatrix Cs_;  // gets inverted in place
   mfem::LUFactors Csinverse_;
   /// @todo there is probably a better way to handle the C_s^{-T}
   mfem::DenseMatrix CsT_;   // gets inverted in place
   mfem::LUFactors CsTinverse_;
   mfem::Array<int> ipiv_;
   mfem::Array<int> ipivT_;
};

EliminationProjection::EliminationProjection(SparseMatrix& A, SparseMatrix& B,
                                             Array<int>& master_contact_dofs,
                                             Array<int>& slave_contact_dofs)
   :
   mfem::Operator(A.Height(),
                  A.Height() - slave_contact_dofs.Size()),
   A_(A),
   B_(B),
   master_contact_dofs_(master_contact_dofs),
   slave_contact_dofs_(slave_contact_dofs)
{
   mfem::Array<int> lm_dofs;
   for (int i = 0; i < B.Height(); ++i)
   {
      lm_dofs.Append(i);
   }
   Cm_.SetSize(B_.Height(), master_contact_dofs.Size());
   B_.GetSubMatrix(lm_dofs, master_contact_dofs, Cm_);

   Cs_.SetSize(B_.Height(), slave_contact_dofs.Size());
   B_.GetSubMatrix(lm_dofs, slave_contact_dofs, Cs_);
   CsT_.Transpose(Cs_);

   ipiv_.SetSize(Cs_.Height());
   Csinverse_.data = Cs_.GetData();
   Csinverse_.ipiv = ipiv_.GetData();
   Csinverse_.Factor(Cs_.Height());

   ipivT_.SetSize(Cs_.Height());
   CsTinverse_.data = CsT_.GetData();
   CsTinverse_.ipiv = ipivT_.GetData();
   CsTinverse_.Factor(Cs_.Height());
}

/**
   Return an assembled approximate version of this projector

   (the current implementation is actually not approximate, but it should be,
   using diagonal or something)

   It *may* be possible to implement this with Cm_ as a findpts call
   rather than a matrix; the hypre assembly will not be so great, though
*/
mfem::SparseMatrix * EliminationProjection::AssembleApproximate() const
{
   int num_elim_dofs = slave_contact_dofs_.Size();

   mfem::SparseMatrix * out = new mfem::SparseMatrix(
      A_.Height(), A_.Height() - num_elim_dofs);

   int column_dof = 0;
   mfem::Array<int> mapped_master_contact_dofs;
   for (int i = 0; i < A_.Height(); ++i)
   {
      if (slave_contact_dofs_.FindSorted(i) >= 0)
      {
         // if dof is a slave, it doesn't show up in reduced system
         ;
      }
      else
      {
         // otherwise, dof exists in reduced system (identity)
         out->Set(i, column_dof, 1.0);
         if (master_contact_dofs_.FindSorted(i) >= 0)
         {
            // in addition, mapped_master_contact_dofs[reduced_id] = larger_id
            mapped_master_contact_dofs.Append(column_dof);
         }
         column_dof++;
      }
   }
   MFEM_ASSERT(mapped_master_contact_dofs.Size() == master_contact_dofs_.Size(),
               "Unable to map master contact dofs!");

   mfem::DenseMatrix block(Cm_);
   std::cout << "        inverting matrix of size " << Cs_.Height() << std::endl;
   Csinverse_.Solve(Cs_.Height(), Cm_.Width(), block.GetData());
   std::cout << "        ...done." << std::endl;

   for (int iz = 0; iz < slave_contact_dofs_.Size(); ++iz)
   {
      int i = slave_contact_dofs_[iz];
      for (int jz = 0; jz < mapped_master_contact_dofs.Size(); ++jz)
      {
         int j = mapped_master_contact_dofs[jz];
         out->Add(i, j, -block(iz, jz));
      }
   }
   out->Finalize();
   return out;
}

void EliminationProjection::Mult(const mfem::Vector& in, mfem::Vector& out) const
{
   int num_elim_dofs = slave_contact_dofs_.Size();
   MFEM_ASSERT(in.Size() == A_.Height() - num_elim_dofs, "Sizes don't match!");
   MFEM_ASSERT(out.Size() == A_.Height(), "Sizes don't match!");

   out = 0.0;
   int column_dof = 0;
   mfem::Array<int> mapped_master_contact_dofs;
   for (int i = 0; i < A_.Height(); ++i)
   {
      if (slave_contact_dofs_.FindSorted(i) >= 0)
      {
         // if dof is a slave, it doesn't show up in reduced system
         ;
      }
      else
      {
         // otherwise, dof exists in reduced system (identity)
         out(i) += in(column_dof);
         if (master_contact_dofs_.FindSorted(i) >= 0)
         {
            // in addition, mapped_master_contact_dofs[reduced_id] = larger_id
            mapped_master_contact_dofs.Append(column_dof);
         }
         column_dof++;
      }
   }
   MFEM_ASSERT(mapped_master_contact_dofs.Size() == master_contact_dofs_.Size(),
               "Unable to map master contact dofs!");

   mfem::Vector subvecin;
   mfem::Vector subvecout(slave_contact_dofs_.Size());
   in.GetSubVector(mapped_master_contact_dofs, subvecin);
   Cm_.Mult(subvecin, subvecout);
   Csinverse_.Solve(Cs_.Height(), 1, subvecout);
   subvecout *= -1.0;
   out.AddElementVector(slave_contact_dofs_, subvecout);
}

void EliminationProjection::MultTranspose(const mfem::Vector& in, mfem::Vector& out) const
{
   int num_elim_dofs = slave_contact_dofs_.Size();
   MFEM_ASSERT(out.Size() == A_.Height() - num_elim_dofs, "Sizes don't match!");
   MFEM_ASSERT(in.Size() == A_.Height(), "Sizes don't match!");

   out = 0.0;
   int row_dof = 0;
   mfem::Array<int> mapped_master_contact_dofs;
   for (int i = 0; i < A_.Height(); ++i)
   {
      if (slave_contact_dofs_.FindSorted(i) >= 0)
      {
         ;
      }
      else
      {
         out(row_dof) += in(i);
         if (master_contact_dofs_.FindSorted(i) >= 0)
         {
            mapped_master_contact_dofs.Append(row_dof);
         }
         row_dof++;
      }
   }
   MFEM_ASSERT(mapped_master_contact_dofs.Size() == master_contact_dofs_.Size(),
               "Unable to map master contact dofs!");

   mfem::Vector subvecin;
   mfem::Vector subvecout(Cm_.Width());

   in.GetSubVector(slave_contact_dofs_, subvecin);
   CsTinverse_.Solve(Cs_.Height(), 1, subvecin);
   Cm_.MultTranspose(subvecin, subvecout);
   subvecout *= -1.0;
   out.AddElementVector(mapped_master_contact_dofs, subvecout);
}

void EliminationProjection::BuildGTilde(const mfem::Vector& g, mfem::Vector& gtilde) const
{
   // int num_elim_dofs = slave_contact_dofs_.Size();
   MFEM_ASSERT(g.Size() == B_.Height(), "Sizes don't match!");
   MFEM_ASSERT(gtilde.Size() == A_.Height(), "Sizes don't match!");

   gtilde = 0.0;
   mfem::Vector cinvg(g);
   Csinverse_.Solve(Cs_.Height(), 1, cinvg);
   gtilde.AddElementVector(slave_contact_dofs_, cinvg);
}

void EliminationProjection::RecoverPressure(const mfem::Vector& disprhs, const mfem::Vector& disp,
                                            mfem::Vector& pressure) const
{
   MFEM_ASSERT(pressure.Size() == B_.Height(), "Sizes don't match!");
   MFEM_ASSERT(disp.Size() == A_.Height(), "Sizes don't match!");

   mfem::Vector fullrhs(A_.Height());
   A_.Mult(disp, fullrhs);
   fullrhs -= disprhs;
   fullrhs *= -1.0;
   fullrhs.GetSubVector(slave_contact_dofs_, pressure);
   CsTinverse_.Solve(Cs_.Height(), 1, pressure);
}



class EliminationCGSolver : public mfem::Solver
{
public:
   EliminationCGSolver(SparseMatrix& A, SparseMatrix& B, int firstblocksize);

   EliminationCGSolver(SparseMatrix& A, SparseMatrix& B, mfem::Array<int>& master_dofs,
                       mfem::Array<int>& slave_dofs);

   ~EliminationCGSolver();

   void SetOperator(const mfem::Operator& op) { }

   void Mult(const mfem::Vector& x, mfem::Vector& y) const;

private:
   /**
      This assumes the master/slave dofs are cleanly separated in
      the matrix, and the given index tells you where.

      We want to move away from this assumption, the first step
      is to separate its logic in this method.
   */
   void BuildSeparatedInterfaceDofs(int firstblocksize);

   void BuildPreconditioner();

   SparseMatrix& A_;
   SparseMatrix& B_;
   Array<int> first_interface_dofs_;
   Array<int> second_interface_dofs_;
   EliminationProjection * projector_;
   HypreParMatrix * h_explicit_operator_;
   HypreBoomerAMG * prec_;
};


EliminationCGSolver::~EliminationCGSolver()
{
   delete h_explicit_operator_;
   delete projector_;
   delete prec_;
}

void EliminationCGSolver::BuildSeparatedInterfaceDofs(int firstblocksize)
{
   std::set<int> first_interface;
   std::set<int> second_interface;
   int * I = B_.GetI();
   int * J = B_.GetJ();
   double * data = B_.GetData();
   const double tol = 1.e-14;
   for (int i = 0; i < B_.Height(); ++i)
   {
      for (int jidx = I[i]; jidx < I[i + 1]; ++jidx)
      {
         int j = J[jidx];
         double v = data[jidx];
         if (fabs(v) < tol)
         {
            continue;
         }
         if (j < firstblocksize)
         {
            first_interface.insert(j);
         }
         else
         {
            second_interface.insert(j);
         }
      }
   }

   for (auto i : first_interface)
   {
      first_interface_dofs_.Append(i);
   }
   first_interface_dofs_.Sort();
   int second_interface_size = 0;
   for (auto i : second_interface)
   {
      second_interface_size++;
      second_interface_dofs_.Append(i);
   }
   second_interface_dofs_.Sort();

   if (second_interface_size != B_.Height())
   {
      // is this really expected? equating nodes with dofs in some weird way on a manifold?
      std::cerr << "B_.Height() = " << B_.Height()
                << ", slave_interface_size = " << second_interface_size << std::endl;
      MFEM_VERIFY(false, "I don't understand how this matrix is constructed!");
   }
}

void EliminationCGSolver::BuildPreconditioner()
{
   // first_interface_dofs = master_dofs, column indices corresponding to nonzeros in constraint
   // rectangular C_1 = C_m has lagrange_dofs rows, first_interface_dofs columns
   // square C_2 = C_s has lagrange_dofs rows, second_interface_dofs columns
   projector_ = new EliminationProjection(A_, B_, first_interface_dofs_,
                                          second_interface_dofs_);

   mfem::SparseMatrix * explicit_projector = projector_->AssembleApproximate();
   mfem::HypreParMatrix * h_explicit_projector = SerialHypreMatrix(*explicit_projector);
   mfem::HypreParMatrix * h_A = SerialHypreMatrix(A_, false);
   h_explicit_operator_ = RAP(h_A, h_explicit_projector);
   h_explicit_operator_->CopyRowStarts();
   h_explicit_operator_->CopyColStarts();
   prec_ = new mfem::HypreBoomerAMG(*h_explicit_operator_);
   prec_->SetPrintLevel(0);

   delete explicit_projector;
   delete h_explicit_projector;
   delete h_A;
   // delete explicit_operator;

   // so kind of want to do this, use systems version of AMG, but we've eliminated
   // just some of the dofs associated with a particular direction, so the sizes
   // don't work out! (the *correct* way to do this reorders again or uses rigid body modes
   // or something)
   /*
      mfem::SparseMatrix * explicit_operator = RAPWithP(A, *explicit_projector);
      std::cout << "A.Height() = " << A.Height() << ", explicit_operator->Height() = "
                << explicit_operator->Height() << std::endl;
      const int dim = 3;
      HypreBoomerAMGReordered prec(*explicit_operator, dim);
   */
}

EliminationCGSolver::EliminationCGSolver(SparseMatrix& A, SparseMatrix& B,
                                         mfem::Array<int>& master_dofs,
                                         mfem::Array<int>& slave_dofs)
   :
   A_(A),
   B_(B),
   first_interface_dofs_(master_dofs),
   second_interface_dofs_(slave_dofs)
{
   BuildPreconditioner();
}

EliminationCGSolver::EliminationCGSolver(SparseMatrix& A, SparseMatrix& B,
                                         int firstblocksize)
   :
   mfem::Solver(A.Height() - firstblocksize), // TODO??!
   A_(A),
   B_(B)
{
   // identify interface dofs to eliminate via nonzero structure
   mfem::StopWatch chrono;
   chrono.Start();

   BuildSeparatedInterfaceDofs(firstblocksize);

   BuildPreconditioner();

   chrono.Stop();
   std::cout << "  elimination solver and AMG setup time: " << chrono.RealTime() << std::endl;
}

void EliminationCGSolver::Mult(const mfem::Vector& rhs, mfem::Vector& sol) const
{
   mfem::RAPOperator reducedoperator(*projector_, A_, *projector_);
   mfem::CGSolver krylov;
   krylov.SetOperator(reducedoperator);
   krylov.SetPreconditioner(*prec_);
   krylov.SetMaxIter(1000);
   krylov.SetRelTol(1.e-8);
   krylov.SetPrintLevel(1);

   mfem::Vector displacementrhs(A_.Height());
   for (int i = 0; i < displacementrhs.Size(); ++i)
   {
      displacementrhs(i) = rhs(i);
   }
   mfem::Vector lagrangerhs(B_.Height());
   for (int i = 0; i < lagrangerhs.Size(); ++i)
   {
      lagrangerhs(i) = rhs(displacementrhs.Size() + i);
   }
   mfem::Vector displacementsol(displacementrhs.Size());

   mfem::Vector gtilde(displacementrhs.Size());
   projector_->BuildGTilde(lagrangerhs, gtilde);
   A_.AddMult(gtilde, displacementrhs, -1.0);

   mfem::Vector reducedrhs(reducedoperator.Height());
   projector_->MultTranspose(displacementrhs, reducedrhs);
   mfem::Vector reducedsol(reducedoperator.Height());
   reducedsol = 0.0;
   krylov.Mult(reducedrhs, reducedsol);
   projector_->Mult(reducedsol, displacementsol);

   mfem::Vector pressure(lagrangerhs.Size());
   projector_->RecoverPressure(displacementrhs, displacementsol, pressure);

   displacementsol += gtilde;
   for (int i = 0; i < displacementsol.Size(); ++i)
   {
      sol(i) = displacementsol(i);
   }

   for (int i = 0; i < pressure.Size(); ++i)
   {
      sol(displacementsol.Size() + i) = pressure(i);
   }
}

#endif