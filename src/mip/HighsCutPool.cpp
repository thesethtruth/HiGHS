
#include "mip/HighsCutPool.h"

#include <cassert>

#include "mip/HighsDomain.h"
#include "mip/HighsLpRelaxation.h"
#include "mip/HighsMipSolverData.h"
#include "util/HighsCDouble.h"
#include "util/HighsHash.h"

static size_t support_hash(const int* Rindex, const int Rlen) {
  size_t state = 42;

  for (int i = 0; i != Rlen; ++i) hash_combine(state, size_t(Rindex[i]));

  return state;
}

static void printCut(const int* Rindex, const double* Rvalue, int Rlen,
                     double rhs) {
  for (int i = 0; i != Rlen; ++i) {
    if (Rvalue[i] > 0)
      printf("+%g<x%d> ", Rvalue[i], Rindex[i]);
    else
      printf("-%g<x%d> ", -Rvalue[i], Rindex[i]);
  }

  printf("<= %g\n", rhs);
}

bool HighsCutPool::isDuplicate(size_t hash, double norm, int* Rindex,
                               double* Rvalue, int Rlen, double rhs) {
  auto range = supportmap.equal_range(hash);
  const double* ARvalue = matrix_.getARvalue();
  const int* ARindex = matrix_.getARindex();
  for (auto it = range.first; it != range.second; ++it) {
    int rowindex = it->second;
    int start = matrix_.getRowStart(rowindex);
    int end = matrix_.getRowEnd(rowindex);

    if (end - start != Rlen) continue;
    if (std::equal(Rindex, Rindex + Rlen, &ARindex[start])) {
      HighsCDouble dotprod = 0.0;

      for (int i = 0; i != Rlen; ++i) dotprod += Rvalue[i] * ARvalue[start + i];

      double parallelism = double(dotprod) * rownormalization_[rowindex] * norm;

      // printf("\n\ncuts with same support and parallelism %g:\n",
      // parallelism); printf("CUT1: "); printCut(Rindex, Rvalue, Rlen, rhs);
      // printf("CUT2: ");
      // printCut(Rindex, ARvalue + start, Rlen, rhs_[rowindex]);
      // printf("\n");

      if (parallelism >= 1 - 1e-6) return true;

      //{
      //  if (ages_[rowindex] >= 0) {
      //    matrix_.replaceRowValues(rowindex, Rvalue);
      //    return rowindex;
      //  } else
      //    return -2;
      //}
    }
  }

  return false;
}

double HighsCutPool::getParallelism(int row1, int row2) const {
  int i1 = matrix_.getRowStart(row1);
  const int end1 = matrix_.getRowEnd(row1);

  int i2 = matrix_.getRowStart(row2);
  const int end2 = matrix_.getRowEnd(row2);

  const int* ARindex = matrix_.getARindex();
  const double* ARvalue = matrix_.getARvalue();

  double dotprod = 0.0;
  while (i1 != end1 && i2 != end2) {
    int col1 = ARindex[i1];
    int col2 = ARindex[i2];

    if (col1 < col2)
      ++i1;
    else if (col2 < col1)
      ++i2;
    else {
      dotprod += ARvalue[i1] * ARvalue[i2];
      ++i1;
      ++i2;
    }
  }

  return dotprod * rownormalization_[row1] * rownormalization_[row2];
}

void HighsCutPool::lpCutRemoved(int cut) { ages_[cut] = 1; }

void HighsCutPool::performAging() {
  int numcuts = matrix_.getNumRows();
  for (int i = 0; i != numcuts; ++i) {
    if (ages_[i] < 0) continue;
    ++ages_[i];
    if (ages_[i] > agelim_) {
      ++modification_[i];
      matrix_.removeRow(i);
      ages_[i] = -1;
      rhs_[i] = HIGHS_CONST_INF;
    }
  }
}

void HighsCutPool::separate(const std::vector<double>& sol, HighsDomain& domain,
                            HighsCutSet& cutset, double feastol) {
  int nrows = matrix_.getNumRows();
  const int* ARindex = matrix_.getARindex();
  const double* ARvalue = matrix_.getARvalue();

  assert(cutset.empty());

  std::vector<std::pair<double, int>> efficacious_cuts;

  int agelim = std::min(numSepaRounds, size_t(agelim_));
  ++numSepaRounds;

  for (int i = 0; i < nrows; ++i) {
    // cuts with an age of -1 are already in the LP and are therefore skipped
    if (ages_[i] < 0) continue;

    int start = matrix_.getRowStart(i);
    int end = matrix_.getRowEnd(i);

    HighsCDouble viol(-rhs_[i]);

    for (int j = start; j != end; ++j) {
      int col = ARindex[j];
      double solval = sol[col];

      viol += ARvalue[j] * solval;
    }

    // if the cut is not violated more than feasibility tolerance
    // we skip it and increase its age, otherwise we reset its age
    if (double(viol) <= feastol) {
      ++ages_[i];
      if (ages_[i] >= agelim) {
        size_t sh = support_hash(&ARindex[start], end - start);

        ++modification_[i];

        matrix_.removeRow(i);
        ages_[i] = -1;
        rhs_[i] = 0;
        auto range = supportmap.equal_range(sh);

        for (auto it = range.first; it != range.second; ++it) {
          if (it->second == i) {
            supportmap.erase(it);
            break;
          }
        }
      }
      continue;
    }

    // compute the norm only for those entries that do not sit at their minimal
    // activity in the current solution this avoids the phenomenon that the
    // traditional efficacy gets weaker for stronger cuts E.g. when considering
    // a clique cut which has additional entries whose value in the current
    // solution is 0 then the efficacy gets lower for each such entry even
    // though the cut dominates the clique cut where all those entries are
    // relaxed out.
    HighsCDouble rownorm = 0.0;
    for (int j = start; j != end; ++j) {
      int col = ARindex[j];
      double solval = sol[col];
      if (ARvalue[j] > 0) {
        if (solval - feastol > domain.colLower_[col])
          rownorm += ARvalue[j] * ARvalue[j];
      } else {
        if (solval + feastol < domain.colUpper_[col])
          rownorm += ARvalue[j] * ARvalue[j];
      }
    }

    double sparsity = 1.0 - (end - start) / (double)domain.colLower_.size();
    ages_[i] = 0;
    double efficacy = double(1e-2 * sparsity + viol / sqrt(double(rownorm)));

    efficacious_cuts.emplace_back(efficacy, i);
  }

  std::sort(efficacious_cuts.begin(), efficacious_cuts.end(),
            [](const std::pair<double, int>& a,
               const std::pair<double, int>& b) { return a.first > b.first; });

  int selectednnz = 0;

  assert(cutset.empty());

  for (const std::pair<double, int>& p : efficacious_cuts) {
    bool discard = false;
    double maxpar = 0.1;
    for (int k : cutset.cutindices) {
      if (getParallelism(k, p.second) > maxpar) {
        discard = true;
        break;
      }
    }

    if (discard) continue;

    ages_[p.second] = -1;
    cutset.cutindices.push_back(p.second);
    selectednnz += matrix_.getRowEnd(p.second) - matrix_.getRowStart(p.second);
  }

  cutset.resize(selectednnz);

  assert(int(cutset.ARvalue_.size()) == selectednnz);
  assert(int(cutset.ARindex_.size()) == selectednnz);

  int offset = 0;
  for (int i = 0; i != cutset.numCuts(); ++i) {
    cutset.ARstart_[i] = offset;
    int cut = cutset.cutindices[i];
    int start = matrix_.getRowStart(cut);
    int end = matrix_.getRowEnd(cut);
    cutset.upper_[i] = rhs_[cut];

    for (int j = start; j != end; ++j) {
      assert(offset < selectednnz);
      cutset.ARvalue_[offset] = ARvalue[j];
      cutset.ARindex_[offset] = ARindex[j];
      ++offset;
    }
  }

  cutset.ARstart_[cutset.numCuts()] = offset;
}

int HighsCutPool::addCut(const HighsMipSolver& mipsolver, int* Rindex,
                         double* Rvalue, int Rlen, double rhs, bool integral) {
  mipsolver.mipdata_->debugSolution.checkCut(Rindex, Rvalue, Rlen, rhs);

  size_t sh = support_hash(Rindex, Rlen);
  // compute 1/||a|| for the cut
  // as it is only computed once
  // we use HighsCDouble to compute it as accurately as possible
  HighsCDouble norm = 0.0;
  double maxabscoef = 0.0;
  for (int i = 0; i != Rlen; ++i) {
    norm += Rvalue[i] * Rvalue[i];
    maxabscoef = std::max(maxabscoef, std::abs(Rvalue[i]));
  }
  norm.renormalize();
  double normalization = 1.0 / double(sqrt(norm));
  // try to replace another cut with equal support that has an age > 0

  if (isDuplicate(sh, normalization, Rindex, Rvalue, Rlen, rhs)) return -1;

  // if no such cut exists we append the new cut
  int rowindex = matrix_.addRow(Rindex, Rvalue, Rlen);
  supportmap.emplace(sh, rowindex);

  if (rowindex == int(rhs_.size())) {
    rhs_.resize(rowindex + 1);
    ages_.resize(rowindex + 1);
    modification_.resize(rowindex + 1);
    rownormalization_.resize(rowindex + 1);
    maxabscoef_.resize(rowindex + 1);
    rowintegral.resize(rowindex + 1);
  }

  // set the right hand side and reset the age
  rhs_[rowindex] = rhs;
  ages_[rowindex] = 0;
  rowintegral[rowindex] = integral;
  ++modification_[rowindex];

  rownormalization_[rowindex] = normalization;
  maxabscoef_[rowindex] = maxabscoef;

  for (HighsDomain::CutpoolPropagation* propagationdomain : propagationDomains)
    propagationdomain->cutAdded(rowindex);

  return rowindex;
}