/*
 * Copyright (c) 2011-2019, The DART development contributors
 * All rights reserved.
 *
 * The list of contributors can be found at:
 *   https://github.com/dartsim/dart/blob/master/LICENSE
 *
 * This file is provided under the following "BSD-style" License:
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 *   CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 *   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 *   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 *   USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 *   AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *   LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *   ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *   POSSIBILITY OF SUCH DAMAGE.
 */

#include <chrono>
#include <iostream>
#include <thread>

#include <dart/gui/gui.hpp>
#include <gtest/gtest.h>

#include "dart/collision/CollisionObject.hpp"
#include "dart/collision/Contact.hpp"
#include "dart/dynamics/BodyNode.hpp"
#include "dart/dynamics/RevoluteJoint.hpp"
#include "dart/dynamics/Skeleton.hpp"
#include "dart/gui/glut/TrajectoryReplayWindow.hpp"
#include "dart/math/Geometry.hpp"
#include "dart/neural/BackpropSnapshot.hpp"
#include "dart/neural/ConstrainedGroupGradientMatrices.hpp"
#include "dart/neural/DifferentiableContactConstraint.hpp"
#include "dart/neural/IKMapping.hpp"
#include "dart/neural/IdentityMapping.hpp"
#include "dart/neural/Mapping.hpp"
#include "dart/neural/NeuralConstants.hpp"
#include "dart/neural/NeuralUtils.hpp"
#include "dart/neural/RestorableSnapshot.hpp"
#include "dart/neural/WithRespectToMass.hpp"
#include "dart/simulation/World.hpp"
#include "dart/trajectory/IPOptOptimizer.hpp"
#include "dart/trajectory/MultiShot.hpp"
#include "dart/trajectory/Problem.hpp"
#include "dart/trajectory/SingleShot.hpp"
#include "dart/trajectory/Solution.hpp"
#include "dart/trajectory/TrajectoryConstants.hpp"
#include "dart/trajectory/TrajectoryRollout.hpp"

#include "GradientTestUtils.hpp"
#include "TestHelpers.hpp"
#include "stdio.h"

// #define ALL_TESTS

using namespace dart;
using namespace math;
using namespace dynamics;
using namespace simulation;
using namespace neural;
using namespace trajectory;

bool equals(TimestepJacobians a, TimestepJacobians b, double threshold)
{
  return equals(a.forcePos, b.forcePos, threshold)
         && equals(a.forceVel, b.forceVel, threshold)
         && equals(a.posPos, b.posPos, threshold)
         && equals(a.posVel, b.posVel, threshold)
         && equals(a.velPos, b.velPos, threshold)
         && equals(a.velVel, b.velVel, threshold);
}

void debugMatrices(
    Eigen::MatrixXd analytical,
    Eigen::MatrixXd bruteForce,
    double threshold,
    std::string name)
{
  if (!equals(analytical, bruteForce, threshold))
  {
    std::cout << "Error at " << name << ":" << std::endl;
    std::cout << "Analytical:" << std::endl << analytical << std::endl;
    std::cout << "Brute force:" << std::endl << bruteForce << std::endl;
    std::cout << "Diff:" << std::endl << (analytical - bruteForce) << std::endl;
  }
}

bool verifySingleStep(WorldPtr world, double EPS)
{
  LossFn lossFn = LossFn();
  SingleShot shot(world, lossFn, 1);
  TimestepJacobians analyticalJacobians
      = shot.backpropStartStateJacobians(world);
  TimestepJacobians bruteForceJacobians
      = shot.finiteDifferenceStartStateJacobians(world, EPS);
  BackpropSnapshotPtr ptr = neural::forwardPass(world);
  Eigen::MatrixXd velVelAnalytical = ptr->getVelVelJacobian(world);
  Eigen::MatrixXd velVelFD = ptr->finiteDifferenceVelVelJacobian(world);

  Eigen::MatrixXd forceVel = ptr->getForceVelJacobian(world);
  Eigen::MatrixXd forceVelFD = ptr->finiteDifferenceForceVelJacobian(world);

  Eigen::MatrixXd velCJacobian = ptr->getVelCJacobian(world);

  double threshold = 1e-8;

  if (!equals(analyticalJacobians.velVel, bruteForceJacobians.velVel, threshold)
      || !equals(velVelAnalytical, velVelFD, threshold)
      || !equals(forceVel, forceVelFD, threshold))
  {
    std::cout << "Time series: " << std::endl;
    debugMatrices(
        analyticalJacobians.velVel,
        bruteForceJacobians.velVel,
        threshold,
        "v_t -> v_end");

    std::cout << "Jacobians: " << std::endl;
    debugMatrices(velVelAnalytical, velVelFD, threshold, "v_t -> v_end");
    debugMatrices(forceVel, forceVelFD, threshold, "f_t -> v_end");
    std::cout << "Vel-C: " << std::endl
              << ptr->getVelCJacobian(world) << std::endl;

    return false;
  }
  return true;
}

bool verifySingleShot(
    WorldPtr world,
    int maxSteps,
    double EPS,
    bool useFdJacs,
    std::shared_ptr<Mapping> mapping)
{
  for (int i = 1; i < maxSteps; i++)
  {
    LossFn lossFn = LossFn();
    SingleShot shot(world, lossFn, i);
    if (mapping != nullptr)
    {
      shot.addMapping("custom", mapping);
      shot.switchRepresentationMapping(world, "custom");
    }

    double threshold = 1e-8;
    std::vector<MappedBackpropSnapshotPtr> ptrs = shot.getSnapshots(world);
    /*
    for (int j = 0; j < ptrs.size(); j++)
    {
      if (!useFdJacs
          && (!equals(
                  ptrs[j]->getPosPosJacobian(world),
                  ptrs[j]->finiteDifferencePosPosJacobian(world, 1),
                  threshold)
              || !equals(
                  ptrs[j]->getVelPosJacobian(world),
                  ptrs[j]->finiteDifferenceVelPosJacobian(world, 1),
                  threshold)
              || !equals(
                  ptrs[j]->getPosVelJacobian(world),
                  ptrs[j]->finiteDifferencePosVelJacobian(world),
                  threshold)
              || !equals(
                  ptrs[j]->getVelVelJacobian(world),
                  ptrs[j]->finiteDifferenceVelVelJacobian(world),
                  threshold)
              || !equals(
                  ptrs[j]->getForceVelJacobian(world),
                  ptrs[j]->finiteDifferenceForceVelJacobian(world),
                  threshold)))
      {
        std::cout << "Detected Jac imprecision at step " << (j + 1) << "/" << i
                  << std::endl;
        debugMatrices(
            ptrs[j]->getPosPosJacobian(world),
            ptrs[j]->finiteDifferencePosPosJacobian(world, 1),
            threshold,
            "pos-pos jac");
        debugMatrices(
            ptrs[j]->getVelPosJacobian(world),
            ptrs[j]->finiteDifferenceVelPosJacobian(world, 1),
            threshold,
            "vel-pos jac");
        debugMatrices(
            ptrs[j]->getPosVelJacobian(world),
            ptrs[j]->finiteDifferencePosVelJacobian(world),
            threshold,
            "pos-vel jac");
        debugMatrices(
            ptrs[j]->getVelVelJacobian(world),
            ptrs[j]->finiteDifferenceVelVelJacobian(world),
            threshold,
            "vel-vel jac");
        debugMatrices(
            ptrs[j]->getForceVelJacobian(world),
            ptrs[j]->finiteDifferenceForceVelJacobian(world),
            threshold,
            "force-vel jac");

        world->setPositions(ptrs[j]->getPreStepPosition());
        world->setVelocities(ptrs[j]->getPreStepVelocity());
        world->setForces(ptrs[j]->getPreStepTorques());
        verifyVelGradients(world, ptrs[j]->getPreStepVelocity());

        return false;
      }
    }
    */

    TimestepJacobians analyticalJacobians
        = shot.backpropStartStateJacobians(world, useFdJacs);
    TimestepJacobians bruteForceJacobians
        = shot.finiteDifferenceStartStateJacobians(world, 1e-7);
    if (!equals(analyticalJacobians, bruteForceJacobians, threshold))
    {
      std::cout << "Trajectory broke at timestep " << i << ":" << std::endl;
      debugMatrices(
          analyticalJacobians.forcePos,
          bruteForceJacobians.forcePos,
          threshold,
          "f_t -> p_end");
      debugMatrices(
          analyticalJacobians.forceVel,
          bruteForceJacobians.forceVel,
          threshold,
          "f_t -> v_end");
      debugMatrices(
          analyticalJacobians.posPos,
          bruteForceJacobians.posPos,
          threshold,
          "p_t -> p_end");
      debugMatrices(
          analyticalJacobians.posVel,
          bruteForceJacobians.posVel,
          threshold,
          "p_t -> v_end");
      debugMatrices(
          analyticalJacobians.velPos,
          bruteForceJacobians.velPos,
          threshold,
          "v_t -> p_end");
      debugMatrices(
          analyticalJacobians.velVel,
          bruteForceJacobians.velVel,
          threshold,
          "v_t -> v_end");
      return false;
    }
  }
  return true;
}

bool verifyShotJacobian(
    WorldPtr world, int steps, std::shared_ptr<Mapping> mapping)
{
  LossFn lossFn = LossFn();
  SingleShot shot(world, lossFn, steps, true);
  int stateSize = world->getNumDofs() * 2;
  if (mapping != nullptr)
  {
    shot.addMapping("custom", mapping);
    shot.switchRepresentationMapping(world, "custom");
    stateSize = mapping->getPosDim() + mapping->getVelDim();
  }

  int dim = shot.getFlatProblemDim(world);

  // Random initialization
  /*
  srand(42);
  Eigen::VectorXd randomInit = Eigen::VectorXd::Random(dim);
  shot.unflatten(randomInit);
  */

  Eigen::MatrixXd analyticalJacobian = Eigen::MatrixXd::Zero(stateSize, dim);
  shot.backpropJacobianOfFinalState(world, analyticalJacobian);
  Eigen::MatrixXd bruteForceJacobian = Eigen::MatrixXd::Zero(stateSize, dim);
  shot.finiteDifferenceJacobianOfFinalState(world, bruteForceJacobian);
  double threshold = 1e-8;
  if (!equals(analyticalJacobian, bruteForceJacobian, threshold))
  {
    std::cout << "Jacobians don't match!" << std::endl;
    std::cout << "Analytical:" << std::endl << analyticalJacobian << std::endl;
    std::cout << "Brute Force:" << std::endl << bruteForceJacobian << std::endl;
    std::cout << "Diff:" << std::endl
              << (analyticalJacobian - bruteForceJacobian) << std::endl;
    return false;
  }
  return true;
}

bool verifyShotGradient(
    WorldPtr world,
    int steps,
    TrajectoryLossFn loss,
    TrajectoryLossFnAndGrad lossGrad)
{
  LossFn lossFn = LossFn(loss, lossGrad);
  SingleShot shot(world, lossFn, steps, true);
  int dim = shot.getFlatProblemDim(world);

  // Random initialization
  /*
  srand(42);
  Eigen::VectorXd randomInit = Eigen::VectorXd::Random(dim);
  shot.unflatten(randomInit);
  */

  Eigen::VectorXd analyticalGrad = Eigen::VectorXd::Zero(dim);
  shot.backpropGradient(world, analyticalGrad);
  Eigen::VectorXd bruteForceGrad = Eigen::VectorXd::Zero(dim);
  shot.finiteDifferenceGradient(world, bruteForceGrad);

  // This threshold is just barely enough for the cartpole example, but the
  // fluctuation appears due to tuning EPS values for finite differencing, which
  // means I think we're within safe ranges of correct.
  double threshold = 2e-8;
  if (!equals(analyticalGrad, bruteForceGrad, threshold))
  {
    std::cout << "Gradients don't match!" << std::endl;
    std::cout << "Analytical:" << std::endl << analyticalGrad << std::endl;
    std::cout << "Brute Force:" << std::endl << bruteForceGrad << std::endl;
    std::cout << "Diff:" << std::endl
              << (analyticalGrad - bruteForceGrad) << std::endl;
    return false;
  }
  return true;
}

bool verifyMultiShotJacobian(
    WorldPtr world, int steps, int shotLength, std::shared_ptr<Mapping> mapping)
{
  LossFn lossFn = LossFn();
  MultiShot shot(world, lossFn, steps, shotLength, true);
  if (mapping != nullptr)
  {
    shot.addMapping("custom", mapping);
    shot.switchRepresentationMapping(world, "custom");
  }

  int dim = shot.getFlatProblemDim(world);
  int numConstraints = shot.getConstraintDim();

  // Random initialization
  /*
  srand(42);
  Eigen::VectorXd randomInit = Eigen::VectorXd::Random(dim);
  shot.unflatten(randomInit);
  */

  /*
  Eigen::VectorXd pos = randomInit.segment(20, 5);
  Eigen::VectorXd vel = randomInit.segment(25, 5);
  */

  Eigen::MatrixXd analyticalJacobian
      = Eigen::MatrixXd::Zero(numConstraints, dim);
  shot.Problem::backpropJacobian(world, analyticalJacobian);
  Eigen::MatrixXd bruteForceJacobian
      = Eigen::MatrixXd::Zero(numConstraints, dim);
  shot.finiteDifferenceJacobian(world, bruteForceJacobian);
  double threshold = 1e-8;
  if (!equals(analyticalJacobian, bruteForceJacobian, threshold))
  {
    std::cout << "Jacobians don't match!" << std::endl;
    for (int i = 0; i < dim; i++)
    {
      Eigen::VectorXd analyticalCol = analyticalJacobian.col(i);
      Eigen::VectorXd bruteForceCol = bruteForceJacobian.col(i);
      if (!equals(analyticalCol, bruteForceCol, threshold))
      {
        std::cout << "ERROR at col " << shot.getFlatDimName(world, i) << " ("
                  << i << ") by " << (analyticalCol - bruteForceCol).norm()
                  << std::endl;
        /*
        std::cout << "Analytical:" << std::endl << analyticalCol << std::endl;
        std::cout << "Brute Force:" << std::endl << bruteForceCol << std::endl;
        std::cout << "Diff:" << std::endl
                  << (analyticalCol - bruteForceCol) << std::endl;
        */
      }
      else
      {
        std::cout << "Match at col " << shot.getFlatDimName(world, i) << " ("
                  << i << ")" << std::endl;
      }
    }
    return false;
  }
  return true;
}

bool verifySparseJacobian(WorldPtr world, MultiShot& shot)
{
  // Random initialization
  /*
  srand(42);
  Eigen::VectorXd randomInit = Eigen::VectorXd::Random(dim);
  shot.unflatten(randomInit);
  */

  int dim = shot.getFlatProblemDim(world);
  int numConstraints = shot.getConstraintDim();
  Eigen::MatrixXd analyticalJacobian
      = Eigen::MatrixXd::Zero(numConstraints, dim);
  shot.Problem::backpropJacobian(world, analyticalJacobian);
  Eigen::MatrixXd sparseRecoveredJacobian
      = Eigen::MatrixXd::Zero(numConstraints, dim);

  int numSparse = shot.getNumberNonZeroJacobian(world);
  Eigen::VectorXi rows = Eigen::VectorXi::Zero(numSparse);
  Eigen::VectorXi cols = Eigen::VectorXi::Zero(numSparse);
  shot.getJacobianSparsityStructure(world, rows, cols);
  Eigen::VectorXd sparseValues = Eigen::VectorXd::Zero(numSparse);
  shot.Problem::getSparseJacobian(world, sparseValues);
  for (int i = 0; i < numSparse; i++)
  {
    sparseRecoveredJacobian(rows(i), cols(i)) = sparseValues(i);
  }

  double threshold = 0;
  if (!equals(analyticalJacobian, sparseRecoveredJacobian, threshold))
  {
    std::cout << "Sparse jacobians don't match!" << std::endl;
    int staticDim = shot.getFlatStaticProblemDim(world);
    std::cout << "Static region size: " << shot.getFlatStaticProblemDim(world)
              << std::endl;
    std::cout << "Analytical first region: " << std::endl
              << analyticalJacobian.block(0, 0, analyticalJacobian.rows(), 10)
              << std::endl;
    std::cout << "Sparse recovered region: " << std::endl
              << sparseRecoveredJacobian.block(
                     0, 0, analyticalJacobian.rows(), 10)
              << std::endl;

    for (int i = 0; i < dim; i++)
    {
      Eigen::VectorXd analyticalCol = analyticalJacobian.col(i);
      Eigen::VectorXd sparseRecoveredCol = sparseRecoveredJacobian.col(i);
      if (!equals(analyticalCol, sparseRecoveredCol, threshold))
      {
        std::cout << "ERROR at col " << shot.getFlatDimName(world, i) << " ("
                  << i << ") by " << (analyticalCol - sparseRecoveredCol).norm()
                  << std::endl;
        /*
        std::cout << "Dense:" << std::endl << analyticalCol << std::endl;
        std::cout << "Sparse:" << std::endl << sparseRecoveredCol << std::endl;
        std::cout << "Diff:" << std::endl
                  << (analyticalCol - bruteForceCol) << std::endl;
        */
      }
      else
      {
        std::cout << "Match at col " << shot.getFlatDimName(world, i) << " ("
                  << i << ")" << std::endl;
      }
    }
    return false;
  }
  return true;
}

bool verifySparseJacobian(
    WorldPtr world, int steps, int shotLength, std::shared_ptr<Mapping> mapping)
{
  LossFn lossFn = LossFn();
  MultiShot shot(world, lossFn, steps, shotLength, true);
  if (mapping != nullptr)
  {
    shot.addMapping("custom", mapping);
    shot.switchRepresentationMapping(world, "custom");
  }
  return verifySparseJacobian(world, shot);
}

bool verifyMultiShotGradient(
    WorldPtr world,
    int steps,
    int shotLength,
    TrajectoryLossFn loss,
    TrajectoryLossFnAndGrad lossGrad)
{
  LossFn lossFn(loss, lossGrad);
  MultiShot shot(world, lossFn, steps, shotLength, true);

  // Random initialization
  /*
  srand(42);
  Eigen::VectorXd randomInit = Eigen::VectorXd::Random(dim);
  shot.unflatten(randomInit);
  */

  int dim = shot.getFlatProblemDim(world);

  Eigen::VectorXd analyticalGrad = Eigen::VectorXd::Zero(dim);
  shot.backpropGradient(world, analyticalGrad);
  Eigen::VectorXd bruteForceGrad = Eigen::VectorXd::Zero(dim);
  shot.finiteDifferenceGradient(world, bruteForceGrad);

  // This threshold is just barely enough for the cartpole example, but the
  // fluctuation appears due to tuning EPS values for finite differencing, which
  // means I think we're within safe ranges of correct.
  double threshold = 2e-8;
  if (!equals(analyticalGrad, bruteForceGrad, threshold))
  {
    std::cout << "Gradients don't match!" << std::endl;
    std::cout << "Analytical:" << std::endl << analyticalGrad << std::endl;
    std::cout << "Brute Force:" << std::endl << bruteForceGrad << std::endl;
    std::cout << "Diff:" << std::endl
              << (analyticalGrad - bruteForceGrad) << std::endl;
    return false;
  }
  return true;
}

bool verifyMultiShotJacobianCustomConstraint(
    WorldPtr world,
    int steps,
    int shotLength,
    TrajectoryLossFn constraint,
    TrajectoryLossFnAndGrad constraintGrad,
    double constraintValue)
{
  LossFn lossFn = LossFn();
  MultiShot shot(world, lossFn, steps, shotLength, true);

  LossFn constraintFn = LossFn(constraint, constraintGrad);
  constraintFn.setLowerBound(constraintValue);
  constraintFn.setUpperBound(constraintValue);
  shot.addConstraint(constraintFn);

  int dim = shot.getFlatProblemDim(world);
  int numConstraints = shot.getConstraintDim();

  Eigen::MatrixXd analyticalJacobian
      = Eigen::MatrixXd::Zero(numConstraints, dim);
  shot.Problem::backpropJacobian(world, analyticalJacobian);
  Eigen::MatrixXd bruteForceJacobian
      = Eigen::MatrixXd::Zero(numConstraints, dim);
  shot.finiteDifferenceJacobian(world, bruteForceJacobian);
  double threshold = 1e-8;
  if (!equals(analyticalJacobian, bruteForceJacobian, threshold))
  {
    std::cout << "Jacobians don't match!" << std::endl;
    for (int i = 0; i < dim; i++)
    {
      Eigen::VectorXd analyticalCol = analyticalJacobian.col(i);
      Eigen::VectorXd bruteForceCol = bruteForceJacobian.col(i);
      if (!equals(analyticalCol, bruteForceCol, threshold))
      {
        std::cout << "ERROR at col " << shot.getFlatDimName(world, i) << " ("
                  << i << ") by " << (analyticalCol - bruteForceCol).norm()
                  << std::endl;
        /*
        std::cout << "Analytical:" << std::endl << analyticalCol << std::endl;
        std::cout << "Brute Force:" << std::endl << bruteForceCol << std::endl;
        std::cout << "Diff:" << std::endl
                  << (analyticalCol - bruteForceCol) << std::endl;
        */
      }
      else
      {
        std::cout << "Match at col " << shot.getFlatDimName(world, i) << " ("
                  << i << ")" << std::endl;
      }
    }
    return false;
  }
  return true;
}

bool verifyChangeRepresentationToIK(
    WorldPtr world,
    int steps,
    int shotLength,
    std::shared_ptr<IKMapping> newRepresentation,
    bool shouldBeLosslessInto,
    bool shouldBeLosslessOut)
{
  LossFn lossFn = LossFn();
  MultiShot shot(world, lossFn, steps, shotLength, true);

  // Get the initial state
  TrajectoryRolloutReal initialIdentityRollout = TrajectoryRolloutReal(&shot);
  shot.getStates(world, &initialIdentityRollout, nullptr, true);

  shot.addMapping("custom", newRepresentation);
  // Switch to a mapped state, and get the problem state
  shot.switchRepresentationMapping(world, "custom");

  TrajectoryRolloutReal mappedRollout = TrajectoryRolloutReal(&shot);
  shot.getStates(world, &mappedRollout, nullptr, true);

  // Go back to identity maps
  shot.switchRepresentationMapping(world, "identity");

  TrajectoryRolloutReal recoveredIdentityRollout = TrajectoryRolloutReal(&shot);
  shot.getStates(world, &recoveredIdentityRollout, nullptr, true);

  double threshold = 1e-8;

  if (shouldBeLosslessInto)
  {
    for (int i = 0; i < steps; i++)
    {
      world->setPositions(initialIdentityRollout.getPoses("identity").col(i));
      world->setVelocities(initialIdentityRollout.getVels("identity").col(i));
      world->setExternalForces(
          initialIdentityRollout.getForces("identity").col(i));

      Eigen::VectorXd manualMappedPos = newRepresentation->getPositions(world);
      Eigen::VectorXd manualMappedVel = newRepresentation->getVelocities(world);
      Eigen::VectorXd manualMappedForce = newRepresentation->getForces(world);
      Eigen::VectorXd mappedPos = mappedRollout.getPoses("custom").col(i);
      Eigen::VectorXd mappedVel = mappedRollout.getVels("custom").col(i);
      Eigen::VectorXd mappedForce = mappedRollout.getForces("custom").col(i);

      if (!equals(mappedPos, manualMappedPos, threshold)
          || !equals(mappedVel, manualMappedVel, threshold)
          || !equals(mappedForce, manualMappedForce, threshold))
      {
        std::cout << "verifyChangeRepresentationToIK() failed to be lossloss "
                     "in the into mapping "
                     "when shouldBeLosslessInto=true"
                  << std::endl;
        return false;
      }
    }
  }

  if (shouldBeLosslessOut)
  {
    for (int i = 0; i < steps; i++)
    {
      Eigen::VectorXd mappedPos = mappedRollout.getPoses("custom").col(i);
      Eigen::VectorXd mappedVel = mappedRollout.getVels("custom").col(i);
      Eigen::VectorXd mappedForce = mappedRollout.getForces("custom").col(i);
      newRepresentation->setPositions(world, mappedPos);
      newRepresentation->setVelocities(world, mappedVel);
      newRepresentation->setForces(world, mappedForce);

      Eigen::VectorXd recoveredPos
          = recoveredIdentityRollout.getPoses("identity").col(i);
      Eigen::VectorXd recoveredVel
          = recoveredIdentityRollout.getVels("identity").col(i);
      Eigen::VectorXd recoveredForce
          = recoveredIdentityRollout.getForces("identity").col(i);
      Eigen::VectorXd manualRecoveredPos = world->getPositions();
      Eigen::VectorXd manualRecoveredVel = world->getVelocities();
      Eigen::VectorXd manualRecoveredForce = world->getExternalForces();

      if (!equals(recoveredPos, manualRecoveredPos, threshold)
          || !equals(recoveredVel, manualRecoveredVel, threshold)
          || !equals(recoveredForce, manualRecoveredForce, threshold))
      {
        std::cout << "verifyChangeRepresentationToIK() failed to be lossloss "
                     "in the out mapping "
                     "when shouldBeLosslessOut=true"
                  << std::endl;
        std::cout << "Step " << i << ":" << std::endl;
        if (!equals(recoveredPos, manualRecoveredPos, threshold))
        {
          std::cout << "Recovered pos:" << std::endl
                    << recoveredPos << std::endl;
          std::cout << "Manually recovered pos:" << std::endl
                    << manualRecoveredPos << std::endl;
        }
        if (!equals(recoveredVel, manualRecoveredVel, threshold))
        {
          std::cout << "Recovered vel:" << std::endl
                    << recoveredVel << std::endl;
          std::cout << "Manually recovered vel:" << std::endl
                    << manualRecoveredVel << std::endl;
        }
        if (!equals(recoveredForce, manualRecoveredForce, threshold))
        {
          std::cout << "Recovered force:" << std::endl
                    << recoveredForce << std::endl;
          std::cout << "Manually recovered force:" << std::endl
                    << manualRecoveredForce << std::endl;
        }
        return false;
      }
    }
  }

  if (shouldBeLosslessInto && shouldBeLosslessOut)
  {
    if (!equals(
            initialIdentityRollout.getPoses("identity"),
            recoveredIdentityRollout.getPoses("identity"),
            threshold)
        || !equals(
            initialIdentityRollout.getVels("identity"),
            recoveredIdentityRollout.getVels("identity"),
            threshold)
        || !equals(
            initialIdentityRollout.getForces("identity"),
            recoveredIdentityRollout.getForces("identity"),
            threshold))
    {
      std::cout << "verifyChangeRepresentationToIK() failed to be lossloss "
                   "when shouldBeLosslessInto=true and shouldBeLosslessOut=true"
                << std::endl;
      return false;
    }
  }

  return true;
}

//==============================================================================
class AbstractShotWindow : public dart::gui::glut::SimWindow
{
public:
  /// Constructor
  AbstractShotWindow(
      std::shared_ptr<simulation::World> world,
      Eigen::MatrixXd posesWithKnots,
      Eigen::MatrixXd posesWithoutKnots)
  {
    mPosesWithKnots = posesWithKnots;
    mPosesWithoutKnots = posesWithoutKnots;
    mCounter = 0;
    setWorld(world);
  }

  void timeStepping() override
  {
    // std::cout << "Time stepping " << mCounter << std::endl;
    mCounter++;
    int cols = mPosesWithKnots.cols();
    if (mCounter < cols)
      mWorld->setPositions(mPosesWithKnots.col(mCounter));
    if (mCounter >= 2 * cols && mCounter < 3 * cols)
      mWorld->setPositions(mPosesWithoutKnots.col(mCounter - 2 * cols));
    if (mCounter >= 4 * cols)
      mCounter = 0;

    // Step the simulation forward
    SimWindow::draw();
    // SimWindow::timeStepping();
  }

private:
  int mCounter = 0;
  Eigen::MatrixXd mPosesWithKnots;
  Eigen::MatrixXd mPosesWithoutKnots;
};

bool verifyMultiShotOptimization(WorldPtr world, MultiShot shot)
{
  IPOptOptimizer optimizer = IPOptOptimizer();

  optimizer.setIterationLimit(1);
  std::shared_ptr<Solution> record = optimizer.optimize(&shot);
  EXPECT_TRUE(record->getNumSteps() == 2);
  EXPECT_TRUE(record->getStep(0).index == 0);
  EXPECT_TRUE(record->getStep(1).index == 1);
  EXPECT_TRUE(record->getStep(1).rollout != record->getStep(0).rollout);

  // Playback the trajectory

  TrajectoryRolloutReal withKnots = TrajectoryRolloutReal(&shot);
  TrajectoryRolloutReal withoutKnots = TrajectoryRolloutReal(&shot);

  // Get the version with knots
  shot.getStates(world, &withKnots, nullptr, true);
  // Get the version without knots next, so that they can play in a loop
  shot.getStates(world, &withoutKnots, nullptr, false);

  // Create a window for rendering the world and handling user input
  // dart::gui::glut::displayTrajectoryInGUI(world, &shot);
}

#ifdef ALL_TESTS
TEST(TRAJECTORY, UNCONSTRAINED_BOX)
{
  // World
  WorldPtr world = World::create();
  world->setGravity(Eigen::Vector3d(0, -9.81, 0));

  ///////////////////////////////////////////////
  // Create the box
  ///////////////////////////////////////////////

  SkeletonPtr box = Skeleton::create("box");

  std::pair<TranslationalJoint2D*, BodyNode*> pair
      = box->createJointAndBodyNodePair<TranslationalJoint2D>(nullptr);
  TranslationalJoint2D* boxJoint = pair.first;
  BodyNode* boxBody = pair.second;

  boxJoint->setXYPlane();
  boxJoint->setTransformFromParentBodyNode(Eigen::Isometry3d::Identity());
  boxJoint->setTransformFromChildBodyNode(Eigen::Isometry3d::Identity());

  std::shared_ptr<BoxShape> boxShape(
      new BoxShape(Eigen::Vector3d(1.0, 1.0, 1.0)));
  boxBody->createShapeNodeWith<VisualAspect, CollisionAspect>(boxShape);
  boxBody->setFrictionCoeff(0.0);

  // Add a force driving the box to the left
  boxBody->addExtForce(Eigen::Vector3d(1, -1, 0));
  // Prevent the mass matrix from being Identity
  boxBody->setMass(1.0);
  boxBody->setRestitutionCoeff(0.5);
  // Set the 1th joint index to -1.0
  box->setVelocity(1, -1);

  world->addSkeleton(box);

  // Passes
  EXPECT_TRUE(verifySingleStep(world, 1e-7));
  EXPECT_TRUE(verifySingleShot(world, 40, 1e-7, false, nullptr));
  EXPECT_TRUE(verifyShotJacobian(world, 40, nullptr));
  EXPECT_TRUE(verifyMultiShotJacobian(world, 8, 2, nullptr));

  // Verify using the IK mapping as the representation
  std::shared_ptr<IKMapping> ikMap = std::make_shared<IKMapping>(world);
  ikMap->addLinearBodyNode(boxBody);
  EXPECT_TRUE(verifyChangeRepresentationToIK(world, 10, 5, ikMap, true, true));
  EXPECT_TRUE(verifySingleShot(world, 40, 1e-7, false, ikMap));
  EXPECT_TRUE(verifyShotJacobian(world, 40, ikMap));
  EXPECT_TRUE(verifyMultiShotJacobian(world, 8, 2, ikMap));
}
#endif

#ifdef ALL_TESTS
TEST(TRAJECTORY, REVOLUTE_JOINT)
{
  // World
  WorldPtr world = World::create();
  world->setGravity(Eigen::Vector3d(0, -9.81, 0));

  SkeletonPtr spinner = Skeleton::create("spinner");

  std::pair<RevoluteJoint*, BodyNode*> armPair
      = spinner->createJointAndBodyNodePair<RevoluteJoint>(nullptr);
  armPair.first->setAxis(Eigen::Vector3d(0, 0, 1));

  world->addSkeleton(spinner);

  spinner->setPosition(0, 15.0 / 180.0 * 3.1415);
  spinner->computeForwardDynamics();
  spinner->integrateVelocities(world->getTimeStep());

  // Passes
  EXPECT_TRUE(verifySingleStep(world, 1e-7));
  EXPECT_TRUE(verifySingleShot(world, 40, 1e-7, false, nullptr));
  EXPECT_TRUE(verifyShotJacobian(world, 40, nullptr));
  EXPECT_TRUE(verifyMultiShotJacobian(world, 8, 2, nullptr));

  // Verify using the IK mapping as the representation
  std::shared_ptr<IKMapping> ikMap = std::make_shared<IKMapping>(world);
  ikMap->addAngularBodyNode(armPair.second);
  EXPECT_TRUE(verifyChangeRepresentationToIK(world, 10, 5, ikMap, true, true));
  EXPECT_TRUE(verifySingleShot(world, 40, 1e-7, false, ikMap));
  EXPECT_TRUE(verifyShotJacobian(world, 40, ikMap));
  EXPECT_TRUE(verifyMultiShotJacobian(world, 8, 2, ikMap));
}
#endif

#ifdef ALL_TESTS
TEST(TRAJECTORY, TWO_LINK)
{
  // World
  WorldPtr world = World::create();
  world->setGravity(Eigen::Vector3d(0, -9.81, 0));

  SkeletonPtr arm = Skeleton::create("arm");

  std::pair<RevoluteJoint*, BodyNode*> armPair
      = arm->createJointAndBodyNodePair<RevoluteJoint>(nullptr);
  armPair.first->setAxis(Eigen::Vector3d(0, 0, 1));

  // Add child arm

  std::pair<RevoluteJoint*, BodyNode*> elbowPair
      = arm->createJointAndBodyNodePair<RevoluteJoint>(armPair.second);
  Eigen::Isometry3d elbowOffset = Eigen::Isometry3d::Identity();
  elbowOffset.translation() = Eigen::Vector3d(0, 1.0, 0);
  elbowPair.first->setTransformFromParentBodyNode(elbowOffset);

  world->addSkeleton(arm);

  arm->setPosition(0, 15.0 / 180.0 * 3.1415);
  arm->computeForwardDynamics();
  arm->integrateVelocities(world->getTimeStep());

  EXPECT_TRUE(verifySingleStep(world, 1e-7));
  // This passes, but is very slow, so we'll skip it for now
  // EXPECT_TRUE(verifySingleShot(world, 40, 1e-7, false));
  EXPECT_TRUE(verifyShotJacobian(world, 40, nullptr));
  EXPECT_TRUE(verifyMultiShotJacobian(world, 8, 2, nullptr));

  // Verify using the IK mapping as the representation
  std::shared_ptr<IKMapping> ikMap = std::make_shared<IKMapping>(world);
  ikMap->addSpatialBodyNode(armPair.second);
  ikMap->addSpatialBodyNode(elbowPair.second);
  EXPECT_TRUE(verifyChangeRepresentationToIK(world, 10, 5, ikMap, true, true));
  EXPECT_TRUE(verifyShotJacobian(world, 40, ikMap));
  EXPECT_TRUE(verifyMultiShotJacobian(world, 8, 2, ikMap));
}
#endif

#ifdef ALL_TESTS
TEST(TRAJECTORY, PRISMATIC)
{

  // World
  WorldPtr world = World::create();
  world->setGravity(Eigen::Vector3d(0, -9.81, 0));

  SkeletonPtr cartpole = Skeleton::create("cartpole");

  std::pair<PrismaticJoint*, BodyNode*> sledPair
      = cartpole->createJointAndBodyNodePair<PrismaticJoint>(nullptr);
  sledPair.first->setAxis(Eigen::Vector3d(1, 1, 0));

  world->addSkeleton(cartpole);

  cartpole->setPosition(0, 0);
  cartpole->computeForwardDynamics();
  cartpole->integrateVelocities(world->getTimeStep());

  // Passes
  EXPECT_TRUE(verifySingleStep(world, 1e-7));
  EXPECT_TRUE(verifySingleShot(world, 40, 1e-7, false, nullptr));
  EXPECT_TRUE(verifyShotJacobian(world, 40, nullptr));
  EXPECT_TRUE(verifyMultiShotJacobian(world, 8, 2, nullptr));

  // EXPECT_TRUE(verifyShotGradient(world, 7, loss));
  // EXPECT_TRUE(verifyMultiShotGradient(world, 8, 4, loss));
  // EXPECT_TRUE(verifyMultiShotJacobianCustomConstraint(
  // world, 8, 4, loss, lossGrad, 3.0));

  // Verify using the IK mapping as the representation
  std::shared_ptr<IKMapping> ikMap = std::make_shared<IKMapping>(world);
  ikMap->addSpatialBodyNode(sledPair.second);
  EXPECT_TRUE(verifyChangeRepresentationToIK(world, 10, 5, ikMap, true, true));
  EXPECT_TRUE(verifyShotJacobian(world, 40, ikMap));
  EXPECT_TRUE(verifyMultiShotJacobian(world, 8, 2, ikMap));
}
#endif

#ifdef ALL_TESTS
TEST(TRAJECTORY, CARTPOLE)
{
  // World
  WorldPtr world = World::create();
  world->setGravity(Eigen::Vector3d(0, -9.81, 0));

  SkeletonPtr cartpole = Skeleton::create("cartpole");

  std::pair<PrismaticJoint*, BodyNode*> sledPair
      = cartpole->createJointAndBodyNodePair<PrismaticJoint>(nullptr);
  sledPair.first->setAxis(Eigen::Vector3d(1, 0, 0));
  std::shared_ptr<BoxShape> sledShapeBox(
      new BoxShape(Eigen::Vector3d(0.05, 0.25, 0.05)));
  ShapeNode* sledShape
      = sledPair.second->createShapeNodeWith<VisualAspect>(sledShapeBox);

  std::pair<RevoluteJoint*, BodyNode*> armPair
      = cartpole->createJointAndBodyNodePair<RevoluteJoint>(sledPair.second);
  armPair.first->setAxis(Eigen::Vector3d(0, 0, 1));
  std::shared_ptr<BoxShape> armShapeBox(
      new BoxShape(Eigen::Vector3d(0.05, 0.25, 0.05)));
  ShapeNode* armShape
      = armPair.second->createShapeNodeWith<VisualAspect>(armShapeBox);

  Eigen::Isometry3d armOffset = Eigen::Isometry3d::Identity();
  armOffset.translation() = Eigen::Vector3d(0, -0.5, 0);
  armPair.first->setTransformFromChildBodyNode(armOffset);

  world->addSkeleton(cartpole);

  cartpole->setForceUpperLimit(0, 0);
  cartpole->setForceLowerLimit(0, 0);
  cartpole->setVelocityUpperLimit(0, 1000);
  cartpole->setVelocityLowerLimit(0, -1000);
  cartpole->setPositionUpperLimit(0, 10);
  cartpole->setPositionLowerLimit(0, -10);

  cartpole->setForceLowerLimit(1, -1000);
  cartpole->setForceUpperLimit(1, 1000);
  cartpole->setVelocityUpperLimit(1, 1000);
  cartpole->setVelocityLowerLimit(1, -1000);
  cartpole->setPositionUpperLimit(1, 10);
  cartpole->setPositionLowerLimit(1, -10);

  cartpole->setPosition(0, 0);
  cartpole->setPosition(1, 15.0 / 180.0 * 3.1415);
  cartpole->computeForwardDynamics();
  cartpole->integrateVelocities(world->getTimeStep());

  TrajectoryLossFn loss = [](const TrajectoryRollout* rollout) {
    int steps = rollout->getPosesConst("identity").cols();
    Eigen::VectorXd lastPos = rollout->getPosesConst("identity").col(steps - 1);
    return rollout->getVelsConst("identity").col(steps - 1).squaredNorm()
           + lastPos.squaredNorm()
           + rollout->getForcesConst("identity").squaredNorm();
  };

  TrajectoryLossFnAndGrad lossGrad = [](const TrajectoryRollout* rollout,
                                        TrajectoryRollout* gradWrtRollout // OUT
                                     ) {
    gradWrtRollout->getPoses("identity").setZero();
    gradWrtRollout->getVels("identity").setZero();
    gradWrtRollout->getForces("identity").setZero();
    int steps = rollout->getPosesConst("identity").cols();
    gradWrtRollout->getPoses("identity").col(steps - 1)
        = 2 * rollout->getPosesConst("identity").col(steps - 1);
    gradWrtRollout->getVels("identity").col(steps - 1)
        = 2 * rollout->getVelsConst("identity").col(steps - 1);
    for (int i = 0; i < steps; i++)
    {
      gradWrtRollout->getForces("identity").col(i)
          = 2 * rollout->getForcesConst("identity").col(i);
    }
    Eigen::VectorXd lastPos = rollout->getPosesConst("identity").col(steps - 1);
    return rollout->getVelsConst("identity").col(steps - 1).squaredNorm()
           + lastPos.squaredNorm()
           + rollout->getForcesConst("identity").squaredNorm();
  };

  EXPECT_TRUE(verifySingleStep(world, 1e-7));
  EXPECT_TRUE(verifySingleShot(world, 40, 1e-7, false, nullptr));
  EXPECT_TRUE(verifyShotJacobian(world, 40, nullptr));
  EXPECT_TRUE(verifyShotGradient(world, 7, loss, lossGrad));
  EXPECT_TRUE(verifyMultiShotJacobian(world, 8, 2, nullptr));
  EXPECT_TRUE(verifyMultiShotGradient(world, 8, 4, loss, lossGrad));
  EXPECT_TRUE(verifyMultiShotJacobianCustomConstraint(
      world, 8, 4, loss, lossGrad, 3.0));
  // LossFn lossFn(loss);
  // MultiShot shot(world, lossFn, 50, 10, false);
  // EXPECT_TRUE(verifyMultiShotOptimization(world, shot));

  // Verify using the IK mapping as the representation
  std::shared_ptr<IKMapping> ikMap = std::make_shared<IKMapping>(world);
  ikMap->addSpatialBodyNode(sledPair.second);
  ikMap->addSpatialBodyNode(armPair.second);
  EXPECT_TRUE(verifyChangeRepresentationToIK(world, 10, 5, ikMap, true, true));
  // EXPECT_TRUE(verifyShotJacobian(world, 40, ikMap));
  // EXPECT_TRUE(verifyMultiShotJacobian(world, 8, 2, ikMap));
}
#endif

BodyNode* createTailSegment(BodyNode* parent, Eigen::Vector3d color)
{
  std::pair<RevoluteJoint*, BodyNode*> poleJointPair
      = parent->createChildJointAndBodyNodePair<RevoluteJoint>();
  RevoluteJoint* poleJoint = poleJointPair.first;
  BodyNode* pole = poleJointPair.second;
  poleJoint->setAxis(Eigen::Vector3d::UnitZ());

  std::shared_ptr<BoxShape> shape(
      new BoxShape(Eigen::Vector3d(0.05, 0.25, 0.05)));
  ShapeNode* poleShape
      = pole->createShapeNodeWith<VisualAspect, CollisionAspect>(shape);
  poleShape->getVisualAspect()->setColor(color);
  poleJoint->setForceUpperLimit(0, 100.0);
  poleJoint->setForceLowerLimit(0, -100.0);
  poleJoint->setVelocityUpperLimit(0, 100.0);
  poleJoint->setVelocityLowerLimit(0, -100.0);
  poleJoint->setPositionUpperLimit(0, 270 * 3.1415 / 180);
  poleJoint->setPositionLowerLimit(0, -270 * 3.1415 / 180);

  Eigen::Isometry3d poleOffset = Eigen::Isometry3d::Identity();
  poleOffset.translation() = Eigen::Vector3d(0, -0.125, 0);
  poleJoint->setTransformFromChildBodyNode(poleOffset);
  poleJoint->setPosition(0, 90 * 3.1415 / 180);

  if (parent->getParentBodyNode() != nullptr)
  {
    Eigen::Isometry3d childOffset = Eigen::Isometry3d::Identity();
    childOffset.translation() = Eigen::Vector3d(0, 0.125, 0);
    poleJoint->setTransformFromParentBodyNode(childOffset);
  }

  return pole;
}

#ifdef ALL_TESTS
TEST(TRAJECTORY, TUNE_SIMPLE_MASS)
{
  // World
  WorldPtr world = World::create();
  world->setGravity(Eigen::Vector3d(0, -9.81, 0));

  world->setPenetrationCorrectionEnabled(false);
  world->setConstraintForceMixingEnabled(false);

  /////////////////////////////////////////////////////////////////////
  // Create the skeleton with a single revolute joint
  /////////////////////////////////////////////////////////////////////

  SkeletonPtr swing = Skeleton::create("swing");

  std::pair<RevoluteJoint*, BodyNode*> poleJointPair
      = swing->createJointAndBodyNodePair<RevoluteJoint>();
  RevoluteJoint* poleJoint = poleJointPair.first;
  BodyNode* pole = poleJointPair.second;
  poleJoint->setAxis(Eigen::Vector3d::UnitZ());

  std::shared_ptr<BoxShape> shape(
      new BoxShape(Eigen::Vector3d(0.05, 0.25, 0.05)));
  ShapeNode* poleShape
      = pole->createShapeNodeWith<VisualAspect, CollisionAspect>(shape);
  poleJoint->setForceUpperLimit(0, 100.0);
  poleJoint->setForceLowerLimit(0, -100.0);
  poleJoint->setVelocityUpperLimit(0, 100.0);
  poleJoint->setVelocityLowerLimit(0, -100.0);
  poleJoint->setPositionUpperLimit(0, 270 * 3.1415 / 180);
  poleJoint->setPositionLowerLimit(0, -270 * 3.1415 / 180);

  // We're going to tune the full inertia properties of the swinging object
  Eigen::VectorXd upperBounds = Eigen::VectorXd::Ones(3) * 2.0;
  Eigen::VectorXd lowerBounds = Eigen::VectorXd::Ones(3) * -2.0;
  world->getWrtMass()->registerNode(
      pole,
      neural::WrtMassBodyNodeEntryType::INERTIA_COM,
      upperBounds,
      lowerBounds);

  Eigen::Isometry3d poleOffset = Eigen::Isometry3d::Identity();
  poleOffset.translation() = Eigen::Vector3d(0, -0.125, 0);
  poleJoint->setTransformFromChildBodyNode(poleOffset);
  poleJoint->setPosition(0, 90 * 3.1415 / 180);

  world->addSkeleton(swing);

  assert(world->getNumDofs() == 1);

  /////////////////////////////////////////////////////////////////////
  // Define straightforward loss
  /////////////////////////////////////////////////////////////////////

  int STEPS = 12;
  int SHOT_LENGTH = 3;
  int GOAL_STEP = 6;
  double GOAL_AT_STEP = 0.1;

  // Get the GOAL_STEP pose as close to GOAL_AT_STEP
  TrajectoryLossFn loss
      = [GOAL_STEP, GOAL_AT_STEP](const TrajectoryRollout* rollout) {
          const Eigen::Ref<const Eigen::MatrixXd> poses
              = rollout->getPosesConst("identity");
          double poseFive = poses(0, GOAL_STEP);
          return (poseFive - GOAL_AT_STEP) * (poseFive - GOAL_AT_STEP);
        };

  // Get the initial pose and final pose as close to each other as possible
  TrajectoryLossFn loopConstraint = [](const TrajectoryRollout* rollout) {
    const Eigen::Ref<const Eigen::MatrixXd> poses
        = rollout->getPosesConst("identity");
    double firstPose = poses(0, 0);
    double lastPose = poses(0, poses.cols() - 1);
    return (firstPose - lastPose) * (firstPose - lastPose);
  };

  /////////////////////////////////////////////////////////////////////
  // Build a trajectory optimization problem
  /////////////////////////////////////////////////////////////////////

  LossFn lossFn = LossFn(loss);

  LossFn constraintFn = LossFn(loopConstraint);
  constraintFn.setLowerBound(0);
  constraintFn.setUpperBound(0);

  WorldPtr worldPar = world->clone();

  MultiShot shot(world, lossFn, STEPS, SHOT_LENGTH, true);
  shot.addConstraint(constraintFn);

  MultiShot shotPar(worldPar, lossFn, STEPS, SHOT_LENGTH, true);
  shotPar.addConstraint(constraintFn);
  shotPar.setParallelOperationsEnabled(true);

  int n = shot.getFlatProblemDim(world);
  int constraintDim = shot.getConstraintDim();
  /*
  Eigen::VectorXd flat = Eigen::VectorXd::Zero(n);
  shot.AbstractShot::flatten(world, flat);
  srand(2);
  flat += Eigen::VectorXd::Random(n) * 0.01;
  std::cout << flat << std::endl;
  shot.AbstractShot::unflatten(world, flat);
  shotPar.AbstractShot::unflatten(world, flat);
  */

  /////////////////////////////////////////////////////////////////////
  // Check Bounds
  /////////////////////////////////////////////////////////////////////

  if (true)
  {
    Eigen::VectorXd upperBound = Eigen::VectorXd::Zero(n);
    Eigen::VectorXd lowerBound = Eigen::VectorXd::Zero(n);
    shot.AbstractShot::getUpperBounds(world, upperBound);
    shot.AbstractShot::getLowerBounds(world, lowerBound);

    Eigen::VectorXd upperBoundPar = Eigen::VectorXd::Zero(n);
    Eigen::VectorXd lowerBoundPar = Eigen::VectorXd::Zero(n);
    shotPar.AbstractShot::getUpperBounds(worldPar, upperBoundPar);
    shotPar.AbstractShot::getLowerBounds(worldPar, lowerBoundPar);

    if (!equals(upperBound, upperBoundPar, 0))
    {
      std::cout << "Upper Bounds aren't exactly the same!" << std::endl;
      std::cout << "Serial first segment:" << std::endl
                << upperBound.segment(0, 10) << std::endl;
      std::cout << "Parallel first segment:" << std::endl
                << upperBoundPar.segment(0, 10) << std::endl;
      EXPECT_TRUE(false);
      return;
    }

    if (!equals(lowerBound, lowerBoundPar, 0))
    {
      std::cout << "Lower Bounds aren't exactly the same!" << std::endl;
      std::cout << "Serial first segment:" << std::endl
                << lowerBound.segment(0, 10) << std::endl;
      std::cout << "Parallel first segment:" << std::endl
                << lowerBoundPar.segment(0, 10) << std::endl;
      EXPECT_TRUE(false);
      return;
    }

    Eigen::VectorXd constraintUpperBound = Eigen::VectorXd::Zero(constraintDim);
    Eigen::VectorXd constraintLowerBound = Eigen::VectorXd::Zero(constraintDim);
    shot.getConstraintUpperBounds(constraintUpperBound);
    shot.getConstraintLowerBounds(constraintLowerBound);

    Eigen::VectorXd constraintUpperBoundPar
        = Eigen::VectorXd::Zero(constraintDim);
    Eigen::VectorXd constraintLowerBoundPar
        = Eigen::VectorXd::Zero(constraintDim);
    shotPar.getConstraintUpperBounds(constraintUpperBoundPar);
    shotPar.getConstraintLowerBounds(constraintLowerBoundPar);

    if (!equals(constraintUpperBound, constraintUpperBoundPar, 0))
    {
      std::cout << "Constraint Upper Bounds aren't exactly the same!"
                << std::endl;
      std::cout << "Serial first segment:" << std::endl
                << constraintUpperBound.segment(0, 10) << std::endl;
      std::cout << "Parallel first segment:" << std::endl
                << constraintUpperBoundPar.segment(0, 10) << std::endl;
      EXPECT_TRUE(false);
      return;
    }

    if (!equals(constraintLowerBound, constraintLowerBoundPar, 0))
    {
      std::cout << "Constraint Lower Bounds aren't exactly the same!"
                << std::endl;
      std::cout << "Serial first segment:" << std::endl
                << constraintLowerBound.segment(0, 10) << std::endl;
      std::cout << "Parallel first segment:" << std::endl
                << constraintLowerBoundPar.segment(0, 10) << std::endl;
      EXPECT_TRUE(false);
      return;
    }
  }

  /////////////////////////////////////////////////////////////////////
  // Check Gradients
  /////////////////////////////////////////////////////////////////////

  if (true)
  {
    Eigen::VectorXd grad = Eigen::VectorXd::Zero(n);
    shot.backpropGradient(world, grad);
    Eigen::VectorXd gradPar = Eigen::VectorXd::Zero(n);
    shotPar.backpropGradient(worldPar, gradPar);

    if (!equals(grad, gradPar, 0))
    {
      std::cout << "Gradients aren't exactly the same!" << std::endl;
      std::cout << "Serial first segment:" << std::endl
                << grad.segment(0, 10) << std::endl;
      std::cout << "Parallel first segment:" << std::endl
                << gradPar.segment(0, 10) << std::endl;
      EXPECT_TRUE(false);
      return;
    }

    int m = shot.getNumberNonZeroJacobian(world);

    Eigen::VectorXd sparseJac = Eigen::VectorXd::Zero(m);
    shot.AbstractShot::getSparseJacobian(world, sparseJac);
    Eigen::VectorXd sparseJacPar = Eigen::VectorXd::Zero(m);
    shotPar.AbstractShot::getSparseJacobian(worldPar, sparseJacPar);

    if (!equals(sparseJac, sparseJacPar, 0))
    {
      std::cout << "Gradients aren't exactly the same!" << std::endl;
      std::cout << "Serial first segment:" << std::endl
                << sparseJac.segment(0, 10) << std::endl;
      std::cout << "Parallel first segment:" << std::endl
                << sparseJacPar.segment(0, 10) << std::endl;
      EXPECT_TRUE(false);
      return;
    }
  }

  /////////////////////////////////////////////////////////////////////
  // Check Jacobians
  /////////////////////////////////////////////////////////////////////

  if (true)
  {
    int dim = shot.getFlatProblemDim(world);
    int numConstraints = shot.getConstraintDim();
    std::cout << "numConstraints: " << numConstraints << std::endl;

    Eigen::MatrixXd analyticalJacobian
        = Eigen::MatrixXd::Zero(numConstraints, dim);
    shot.AbstractShot::backpropJacobian(world, analyticalJacobian);
    Eigen::MatrixXd bruteForceJacobian
        = Eigen::MatrixXd::Zero(numConstraints, dim);
    shot.finiteDifferenceJacobian(world, bruteForceJacobian);
    double threshold = 1e-8;
    if (!equals(analyticalJacobian, bruteForceJacobian, threshold))
    {
      std::cout << "Jacobians don't match!" << std::endl;
      int staticDim = shot.getFlatStaticProblemDim(world);
      std::cout << "Static region size: " << shot.getFlatStaticProblemDim(world)
                << std::endl;
      std::cout << "Analytical first region: " << std::endl
                << analyticalJacobian.block(0, 0, analyticalJacobian.rows(), 10)
                << std::endl;
      std::cout << "Brute force first region: " << std::endl
                << bruteForceJacobian.block(0, 0, analyticalJacobian.rows(), 10)
                << std::endl;
      /*
      for (int i = 0; i < dim; i++)
      {
        Eigen::VectorXd analyticalCol = analyticalJacobian.col(i);
        Eigen::VectorXd bruteForceCol = bruteForceJacobian.col(i);
        if (!equals(analyticalCol, bruteForceCol, threshold))
        {
          std::cout << "ERROR at col " << shot.getFlatDimName(world, i) << " ("
                    << i << ") by " << (analyticalCol - bruteForceCol).norm()
                    << std::endl;
          std::cout << "Analytical:" << std::endl << analyticalCol << std::endl;
          std::cout << "Brute Force:" << std::endl << bruteForceCol <<
      std::endl; std::cout << "Diff:" << std::endl
                    << (analyticalCol - bruteForceCol) << std::endl;
        }
        else
        {
          // std::cout << "Match at col " << shot.getFlatDimName(world, i) << "
      ("
          // << i
          //          << ")" << std::endl;
        }
      }
      */
      EXPECT_TRUE(false);
      return;
    }

    EXPECT_TRUE(verifySparseJacobian(world, shot));
  }

  /////////////////////////////////////////////////////////////////////
  // Verify flat results
  /////////////////////////////////////////////////////////////////////

  Eigen::VectorXd preFlat = Eigen::VectorXd::Zero(n);
  Eigen::VectorXd preFlatPar = Eigen::VectorXd::Zero(n);
  shot.AbstractShot::flatten(world, preFlat);
  shotPar.AbstractShot::flatten(world, preFlatPar);
  if (!equals(preFlat, preFlatPar, 0))
  {
    std::cout << "Pre-optimization flattening doesn't match!" << std::endl;
    std::cout << "Serial pre-flat: " << std::endl << preFlat << std::endl;
    std::cout << "Parallel pre-flat: " << std::endl << preFlatPar << std::endl;
    for (int i = 0; i < preFlat.size(); i++)
    {
      if (preFlat(i) != preFlatPar(i))
      {
        std::cout << "  Mismatch at index " << i << " ("
                  << shot.getFlatDimName(world, i) << ") by "
                  << preFlat(i) - preFlatPar(i) << ": " << preFlat(i) << " vs "
                  << preFlatPar(i) << std::endl;
      }
    }
    EXPECT_TRUE(false);
    return;
  }

  /////////////////////////////////////////////////////////////////////
  // Actually run the optimization
  /////////////////////////////////////////////////////////////////////

  // Actually do the optimization

  int iterationLimit = 10;

  IPOptOptimizer optimizer = IPOptOptimizer();
  optimizer.setIterationLimit(iterationLimit);
  optimizer.setCheckDerivatives(true);
  optimizer.setRecoverBest(false);
  // optimizer.setRecordFullDebugInfo(true);
  std::shared_ptr<OptimizationRecord> record = optimizer.optimize(&shot);

  std::shared_ptr<OptimizationRecord> recordPar = optimizer.optimize(&shotPar);

  Eigen::VectorXd endFlat = Eigen::VectorXd::Zero(n);
  Eigen::VectorXd endFlatPar = Eigen::VectorXd::Zero(n);
  shot.AbstractShot::flatten(world, endFlat);
  shotPar.AbstractShot::flatten(worldPar, endFlatPar);
  if (!equals(endFlat, endFlatPar, 0))
  {
    std::cout << "Results after " << iterationLimit << " steps don't match!"
              << std::endl;
    for (int i = 0; i < endFlat.size(); i++)
    {
      if (endFlat(i) != endFlatPar(i))
      {
        std::cout << "  Mismatch at index " << i << " ("
                  << shot.getFlatDimName(world, i) << ") by "
                  << endFlat(i) - endFlatPar(i) << ": " << endFlat(i) << " vs "
                  << endFlatPar(i) << std::endl;
      }
    }
  }

  for (int i = 0; i < record->getXs().size(); i++)
  {
    Eigen::VectorXd x0 = record->getXs()[i];
    Eigen::VectorXd x0Par = recordPar->getXs()[i];
    if (!equals(x0, x0Par, 0))
    {
      std::cout << "Xs at eval " << i << " don't match!" << std::endl;
      for (int j = 0; j < x0.size(); j++)
      {
        if (x0(j) != x0Par(j))
        {
          std::cout << "  Mismatch at index " << j << " ("
                    << shot.getFlatDimName(world, j) << ") by "
                    << x0(j) - x0Par(j) << ": " << x0(j) << " vs " << x0Par(j)
                    << std::endl;
        }
      }
    }
  }

  for (int i = 0; i < record->getLosses().size(); i++)
  {
    double loss0 = record->getLosses()[i];
    double loss0Par = recordPar->getLosses()[i];
    if (loss0 != loss0Par)
    {
      std::cout << "Losses at eval " << i << " don't match by "
                << loss0 - loss0Par << std::endl;
    }
  }

  for (int i = 0; i < record->getGradients().size(); i++)
  {
    Eigen::VectorXd grad0 = record->getGradients()[i];
    Eigen::VectorXd grad0Par = recordPar->getGradients()[i];
    if (!equals(grad0, grad0Par, 0))
    {
      std::cout << "Gradients at eval " << i << " don't match!" << std::endl;
      for (int j = 0; j < grad0.size(); j++)
      {
        if (grad0(j) != grad0Par(j))
        {
          std::cout << "  Mismatch at index " << j << " ("
                    << shot.getFlatDimName(world, j) << ") by "
                    << grad0(j) - grad0Par(j) << ": " << grad0(j) << " vs "
                    << grad0Par(j) << std::endl;
        }
      }
    }
  }

  for (int i = 0; i < record->getSparseJacobians().size(); i++)
  {
    Eigen::VectorXd jac0 = record->getSparseJacobians()[i];
    Eigen::VectorXd jac0Par = recordPar->getSparseJacobians()[i];

    Eigen::VectorXi jacRows = Eigen::VectorXi::Zero(jac0.size());
    Eigen::VectorXi jacCols = Eigen::VectorXi::Zero(jac0.size());
    shot.getJacobianSparsityStructure(world, jacRows, jacCols);

    if (!equals(jac0, jac0Par, 0))
    {
      std::cout << "Jacobians at eval " << i << " don't match!" << std::endl;
      for (int j = 0; j < jac0.size(); j++)
      {
        double serial = jac0(j);
        double parallel = jac0Par(j);
        if (serial != parallel)
        {
          std::cout << "  Mismatch at " << jacRows(j) << "," << jacCols(j)
                    << " (" << shot.getFlatDimName(world, jacCols(j)) << ") by "
                    << serial - parallel << ": " << serial << " vs " << parallel
                    << std::endl;
        }
      }
    }
  }

  for (int i = 0; i < record->getConstraintValues().size(); i++)
  {
    Eigen::VectorXd con0 = record->getConstraintValues()[i];
    Eigen::VectorXd con0Par = recordPar->getConstraintValues()[i];
    if (!equals(con0, con0Par, 0))
    {
      std::cout << "Constraints at eval " << i << " don't match!" << std::endl;
    }
  }

  // Playback the trajectory
  TrajectoryRolloutReal withKnots = TrajectoryRolloutReal(&shot);
  shot.getStates(world, &withKnots, nullptr, true);
}
#endif

#ifdef ALL_TESTS
TEST(TRAJECTORY, RECOVER_MASS)
{
  // World
  WorldPtr world = World::create();
  world->setGravity(Eigen::Vector3d(0, -9.81, 0));

  world->setPenetrationCorrectionEnabled(false);
  world->setConstraintForceMixingEnabled(false);

  /////////////////////////////////////////////////////////////////////
  // Create the skeleton with a single revolute joint
  /////////////////////////////////////////////////////////////////////

  SkeletonPtr box = Skeleton::create("box");

  std::pair<PrismaticJoint*, BodyNode*> boxJointPair
      = box->createJointAndBodyNodePair<PrismaticJoint>();
  PrismaticJoint* boxJoint = boxJointPair.first;
  BodyNode* boxBody = boxJointPair.second;

  std::shared_ptr<BoxShape> shape(
      new BoxShape(Eigen::Vector3d(0.05, 0.25, 0.05)));
  ShapeNode* boxShape
      = boxBody->createShapeNodeWith<VisualAspect, CollisionAspect>(shape);

  // We're going to tune the full inertia properties of the swinging object
  Eigen::VectorXd upperBounds = Eigen::VectorXd::Ones(1) * 5.0;
  Eigen::VectorXd lowerBounds = Eigen::VectorXd::Ones(1) * 0.1;
  world->getWrtMass()->registerNode(
      boxBody,
      neural::WrtMassBodyNodeEntryType::INERTIA_MASS,
      upperBounds,
      lowerBounds);

  world->addSkeleton(box);

  assert(world->getNumDofs() == 1);

  /////////////////////////////////////////////////////////////////////
  // Run one simulation forward to provide the raw material for our loss to try
  // to recover
  /////////////////////////////////////////////////////////////////////

  double TRUE_MASS = 2.5;
  int STEPS = 12;
  int SHOT_LENGTH = 3;
  int GOAL_STEP = 6;
  double GOAL_AT_STEP = 0.1;

  boxBody->setMass(TRUE_MASS);
  Eigen::VectorXd knownForce = Eigen::VectorXd::Ones(1) * 0.1;
  world->setPositions(Eigen::VectorXd::Zero(1));
  world->setVelocities(Eigen::VectorXd::Zero(1));
  world->setTimeStep(1e-1);

  Eigen::VectorXd originalPoses = Eigen::VectorXd::Zero(STEPS);
  for (int i = 0; i < STEPS; i++)
  {
    world->setForces(knownForce);
    world->step();
    originalPoses(i) = world->getPositions()[0];
  }

  // Reset position, scramble mass

  world->setPositions(Eigen::VectorXd::Zero(1));
  world->setVelocities(Eigen::VectorXd::Zero(1));
  boxBody->setMass(0.5);

  /////////////////////////////////////////////////////////////////////
  // Define a loss
  /////////////////////////////////////////////////////////////////////

  // Get the GOAL_STEP pose as close to GOAL_AT_STEP
  TrajectoryLossFn loss
      = [originalPoses, STEPS](const TrajectoryRollout* rollout) {
          const Eigen::Ref<const Eigen::MatrixXd> poses
              = rollout->getPosesConst("identity");
          double sum = 0.0;
          for (int i = 0; i < STEPS; i++)
          {
            double diff = 1e2 * (poses(0, i) - originalPoses(i));
            sum += diff * diff;
          }
          return sum;
        };

  /////////////////////////////////////////////////////////////////////
  // Build a trajectory optimization problem
  /////////////////////////////////////////////////////////////////////

  LossFn lossFn = LossFn(loss);

  MultiShot shot(world, lossFn, STEPS, SHOT_LENGTH, false);

  /////////////////////////////////////////////////////////////////////
  // Pin the forces
  /////////////////////////////////////////////////////////////////////

  for (int i = 0; i < STEPS; i++)
  {
    shot.pinForce(i, knownForce);
  }

  /////////////////////////////////////////////////////////////////////
  // Run optimization
  /////////////////////////////////////////////////////////////////////

  IPOptOptimizer optimizer = IPOptOptimizer();
  optimizer.setIterationLimit(50);
  optimizer.setCheckDerivatives(true);
  optimizer.setTolerance(1e-9);
  // optimizer.setRecordFullDebugInfo(true);
  std::shared_ptr<OptimizationRecord> record = optimizer.optimize(&shot);

  // We should have recovered the mass
  double error = abs(boxBody->getMass() - TRUE_MASS);
  if (error > 1e-7)
  {
    std::cout << "Recovered mass: " << boxBody->getMass() << std::endl;
    std::cout << "Error: " << error << std::endl;
    // Get the trajectory
    TrajectoryRolloutReal withKnots = TrajectoryRolloutReal(&shot);
    shot.getStates(world, &withKnots, nullptr, true);
    std::cout << "Original: " << std::endl
              << originalPoses.transpose() << std::endl;
    std::cout << "Forces: " << std::endl
              << withKnots.getForces("identity") << std::endl;
    std::cout << "Positions: " << std::endl
              << withKnots.getPoses("identity") << std::endl;

    EXPECT_TRUE(error < 1e-7);
  }
}
#endif

#ifdef ALL_TESTS
TEST(TRAJECTORY, CONSTRAINED_CYCLE)
{
  // World
  WorldPtr world = World::create();
  world->setGravity(Eigen::Vector3d(0, -9.81, 0));

  world->setPenetrationCorrectionEnabled(false);
  world->setConstraintForceMixingEnabled(false);

  /////////////////////////////////////////////////////////////////////
  // Create the skeleton with a single revolute joint
  /////////////////////////////////////////////////////////////////////

  SkeletonPtr loop = Skeleton::create("loop");

  std::pair<RevoluteJoint*, BodyNode*> poleJointPair
      = loop->createJointAndBodyNodePair<RevoluteJoint>();
  RevoluteJoint* poleJoint = poleJointPair.first;
  BodyNode* pole = poleJointPair.second;
  poleJoint->setAxis(Eigen::Vector3d::UnitZ());

  std::shared_ptr<BoxShape> shape(
      new BoxShape(Eigen::Vector3d(0.05, 0.25, 0.05)));
  ShapeNode* poleShape
      = pole->createShapeNodeWith<VisualAspect, CollisionAspect>(shape);
  poleJoint->setForceUpperLimit(0, 100.0);
  poleJoint->setForceLowerLimit(0, -100.0);
  poleJoint->setVelocityUpperLimit(0, 100.0);
  poleJoint->setVelocityLowerLimit(0, -100.0);
  poleJoint->setPositionUpperLimit(0, 270 * 3.1415 / 180);
  poleJoint->setPositionLowerLimit(0, -270 * 3.1415 / 180);

  Eigen::Isometry3d poleOffset = Eigen::Isometry3d::Identity();
  poleOffset.translation() = Eigen::Vector3d(0, -0.125, 0);
  poleJoint->setTransformFromChildBodyNode(poleOffset);
  poleJoint->setPosition(0, 90 * 3.1415 / 180);

  world->addSkeleton(loop);

  assert(world->getNumDofs() == 1);

  /////////////////////////////////////////////////////////////////////
  // Define straightforward loss
  /////////////////////////////////////////////////////////////////////

  int STEPS = 12;
  int SHOT_LENGTH = 3;
  int GOAL_STEP = 6;
  double GOAL_AT_STEP = 0.1;

  // Get the GOAL_STEP pose as close to GOAL_AT_STEP
  TrajectoryLossFn loss
      = [GOAL_STEP, GOAL_AT_STEP](const TrajectoryRollout* rollout) {
          const Eigen::Ref<const Eigen::MatrixXd> poses
              = rollout->getPosesConst("identity");
          double poseFive = poses(0, GOAL_STEP);
          return (poseFive - GOAL_AT_STEP) * (poseFive - GOAL_AT_STEP);
        };

  // Get the initial pose and final pose as close to each other as possible
  TrajectoryLossFn loopConstraint = [](const TrajectoryRollout* rollout) {
    const Eigen::Ref<const Eigen::MatrixXd> poses
        = rollout->getPosesConst("identity");
    double firstPose = poses(0, 0);
    double lastPose = poses(0, poses.cols() - 1);
    return (firstPose - lastPose) * (firstPose - lastPose);
  };

  /////////////////////////////////////////////////////////////////////
  // Build a trajectory optimization problem
  /////////////////////////////////////////////////////////////////////

  LossFn lossFn = LossFn(loss);
  MultiShot shot(world, lossFn, STEPS, SHOT_LENGTH, true);

  LossFn constraintFn = LossFn(loopConstraint);
  constraintFn.setLowerBound(0);
  constraintFn.setUpperBound(0);
  shot.addConstraint(constraintFn);

  /////////////////////////////////////////////////////////////////////
  // Check Jacobians
  /////////////////////////////////////////////////////////////////////

  int dim = shot.getFlatProblemDim(world);
  int numConstraints = shot.getConstraintDim();
  std::cout << "numConstraints: " << numConstraints << std::endl;

  Eigen::MatrixXd analyticalJacobian
      = Eigen::MatrixXd::Zero(numConstraints, dim);
  shot.AbstractShot::backpropJacobian(world, analyticalJacobian);
  Eigen::MatrixXd bruteForceJacobian
      = Eigen::MatrixXd::Zero(numConstraints, dim);
  shot.finiteDifferenceJacobian(world, bruteForceJacobian);
  double threshold = 1e-8;
  if (!equals(analyticalJacobian, bruteForceJacobian, threshold))
  {
    std::cout << "Jacobians don't match!" << std::endl;
    for (int i = 0; i < dim; i++)
    {
      Eigen::VectorXd analyticalCol = analyticalJacobian.col(i);
      Eigen::VectorXd bruteForceCol = bruteForceJacobian.col(i);
      if (!equals(analyticalCol, bruteForceCol, threshold))
      {
        std::cout << "ERROR at col " << shot.getFlatDimName(world, i) << " ("
                  << i << ") by " << (analyticalCol - bruteForceCol).norm()
                  << std::endl;
        std::cout << "Analytical:" << std::endl << analyticalCol << std::endl;
        std::cout << "Brute Force:" << std::endl << bruteForceCol << std::endl;
        std::cout << "Diff:" << std::endl
                  << (analyticalCol - bruteForceCol) << std::endl;
      }
      else
      {
        // std::cout << "Match at col " << shot.getFlatDimName(world, i) << " ("
        // << i
        //          << ")" << std::endl;
      }
    }
    EXPECT_TRUE(false);
  }

  EXPECT_TRUE(verifySparseJacobian(world, shot));

  /////////////////////////////////////////////////////////////////////
  // Actually run the optimization
  /////////////////////////////////////////////////////////////////////

  IPOptOptimizer optimizer = IPOptOptimizer();
  optimizer.setIterationLimit(100);
  optimizer.setCheckDerivatives(true);

  // Actually do the optimization
  std::shared_ptr<OptimizationRecord> record = optimizer.optimize(&shot);

  // Playback the trajectory
  TrajectoryRolloutReal withKnots = TrajectoryRolloutReal(&shot);
  shot.getStates(world, &withKnots, nullptr, true);
}
#endif

#ifdef ALL_TESTS
TEST(TRAJECTORY, JUMP_WORM)
{
  bool offGround = false;

  // World
  WorldPtr world = World::create();
  world->setGravity(Eigen::Vector3d(0, -9.81, 0));

  world->setPenetrationCorrectionEnabled(false);
  world->setConstraintForceMixingEnabled(false);

  SkeletonPtr jumpworm = Skeleton::create("jumpworm");

  std::pair<TranslationalJoint2D*, BodyNode*> rootJointPair
      = jumpworm->createJointAndBodyNodePair<TranslationalJoint2D>(nullptr);
  TranslationalJoint2D* rootJoint = rootJointPair.first;
  BodyNode* root = rootJointPair.second;

  std::shared_ptr<BoxShape> shape(new BoxShape(Eigen::Vector3d(0.1, 0.1, 0.1)));
  ShapeNode* rootVisual
      = root->createShapeNodeWith<VisualAspect, CollisionAspect>(shape);
  Eigen::Vector3d black = Eigen::Vector3d::Zero();
  rootVisual->getVisualAspect()->setColor(black);
  rootJoint->setForceUpperLimit(0, 0);
  rootJoint->setForceLowerLimit(0, 0);
  rootJoint->setForceUpperLimit(1, 0);
  rootJoint->setForceLowerLimit(1, 0);
  rootJoint->setVelocityUpperLimit(0, 1000.0);
  rootJoint->setVelocityLowerLimit(0, -1000.0);
  rootJoint->setVelocityUpperLimit(1, 1000.0);
  rootJoint->setVelocityLowerLimit(1, -1000.0);
  rootJoint->setPositionUpperLimit(0, 5);
  rootJoint->setPositionLowerLimit(0, -5);
  rootJoint->setPositionUpperLimit(1, 5);
  rootJoint->setPositionLowerLimit(1, -5);

  BodyNode* tail1 = createTailSegment(
      root, Eigen::Vector3d(182.0 / 255, 223.0 / 255, 144.0 / 255));
  BodyNode* tail2 = createTailSegment(
      tail1, Eigen::Vector3d(223.0 / 255, 228.0 / 255, 163.0 / 255));
  BodyNode* tail3 = createTailSegment(
      tail2, Eigen::Vector3d(221.0 / 255, 193.0 / 255, 121.0 / 255));

  Eigen::VectorXd pos = Eigen::VectorXd(5);
  pos << 0, 0, 90, 90, 45;
  jumpworm->setPositions(pos * 3.1415 / 180);

  world->addSkeleton(jumpworm);

  // Floor

  SkeletonPtr floor = Skeleton::create("floor");

  std::pair<WeldJoint*, BodyNode*> floorJointPair
      = floor->createJointAndBodyNodePair<WeldJoint>(nullptr);
  WeldJoint* floorJoint = floorJointPair.first;
  BodyNode* floorBody = floorJointPair.second;
  Eigen::Isometry3d floorOffset = Eigen::Isometry3d::Identity();
  floorOffset.translation() = Eigen::Vector3d(0, offGround ? -0.7 : -0.56, 0);
  floorJoint->setTransformFromParentBodyNode(floorOffset);
  std::shared_ptr<BoxShape> floorShape(
      new BoxShape(Eigen::Vector3d(2.5, 0.25, 0.5)));
  ShapeNode* floorVisual
      = floorBody->createShapeNodeWith<VisualAspect, CollisionAspect>(
          floorShape);
  floorBody->setFrictionCoeff(0);

  world->addSkeleton(floor);

  rootJoint->setVelocity(1, -0.1);
  Eigen::VectorXd vels = world->getVelocities();

  TrajectoryLossFn loss = [](const TrajectoryRollout* rollout) {
    const Eigen::Ref<const Eigen::MatrixXd> poses
        = rollout->getPosesConst("identity");
    const Eigen::Ref<const Eigen::MatrixXd> vels
        = rollout->getVelsConst("identity");
    const Eigen::Ref<const Eigen::MatrixXd> forces
        = rollout->getForcesConst("identity");

    double maxPos = -1000;
    double minPos = 1000;
    for (int i = 0; i < poses.cols(); i++)
    {
      if (poses(1, i) > maxPos)
      {
        maxPos = poses(1, i);
      }
      if (poses(1, i) < minPos)
      {
        minPos = poses(1, i);
      }
    }
    double peakPosLoss = -(maxPos * maxPos) * (maxPos > 0 ? 1.0 : -1.0);
    double minPosLoss = -(minPos * minPos) * (minPos > 0 ? 1.0 : -1.0);
    double endPos = poses(1, poses.cols() - 1);
    double endPosLoss = -(endPos * endPos) * (endPos > 0 ? 1.0 : -1.0);

    double forceLoss = forces.squaredNorm();

    // return endPosLoss * 100 + forceLoss * 1e-3;
    // return forceLoss;
    return endPosLoss; // + forceLoss;
    // return (100 * peakPosLoss) + (20 * minPosLoss) + endPosLoss;
  };

  TrajectoryLossFnAndGrad lossGrad = [](const TrajectoryRollout* rollout,
                                        TrajectoryRollout* gradWrtRollout // OUT
                                     ) {
    gradWrtRollout->getPoses("identity").setZero();
    gradWrtRollout->getVels("identity").setZero();
    gradWrtRollout->getForces("identity").setZero();
    const Eigen::Ref<const Eigen::MatrixXd> poses
        = rollout->getPosesConst("identity");
    const Eigen::Ref<const Eigen::MatrixXd> vels
        = rollout->getVelsConst("identity");
    const Eigen::Ref<const Eigen::MatrixXd> forces
        = rollout->getForcesConst("identity");

    gradWrtRollout->getPoses("identity")(1, poses.cols() - 1)
        = 2 * poses(1, poses.cols() - 1);
    double endPos = poses(1, poses.cols() - 1);
    double endPosLoss = -(endPos * endPos) * (endPos > 0 ? 1.0 : -1.0);
    return endPosLoss;
  };

  // Make a huge timestep, to try to make the gradients easier to get exactly
  // for finite differencing
  world->setTimeStep(1e-3);

  world->setPenetrationCorrectionEnabled(false);
  world->setConstraintForceMixingEnabled(false);

  // EXPECT_TRUE(verifyVelGradients(world, world->getVelocities()));
  // EXPECT_TRUE(verifyNoMultistepIntereference(world, 10));
  // EXPECT_TRUE(verifyAnalyticalJacobians(world));
  // EXPECT_TRUE(verifyAnalyticalBackprop(world));

  // LossFn lossFn(loss);
  // MultiShot shot(world, lossFn, 100, 20, false);
  // std::shared_ptr<IKMapping> ikMap = std::make_shared<IKMapping>(world);
  // ikMap->addLinearBodyNode(root);
  // shot.addMapping("ik", ikMap);
  // EXPECT_TRUE(verifyMultiShotOptimization(world, shot));

  EXPECT_TRUE(verifySingleStep(world, 5e-7));
  // EXPECT_TRUE(verifySingleShot(world, 40, 5e-7, false));
  EXPECT_TRUE(verifyShotJacobian(world, 4, nullptr));
  EXPECT_TRUE(verifyShotGradient(world, 7, loss, lossGrad));
  EXPECT_TRUE(verifyMultiShotJacobian(world, 6, 2, nullptr));
  EXPECT_TRUE(verifySparseJacobian(world, 8, 2, nullptr));
  EXPECT_TRUE(verifyMultiShotGradient(world, 8, 4, loss, lossGrad));
  EXPECT_TRUE(verifyMultiShotJacobianCustomConstraint(
      world, 8, 4, loss, lossGrad, 3.0));
}
#endif

// #ifdef ALL_TESTS
TEST(TRAJECTORY, REOPTIMIZATION)
{
  bool offGround = false;

  // World
  WorldPtr world = World::create();
  world->setGravity(Eigen::Vector3d(0, -9.81, 0));

  world->setPenetrationCorrectionEnabled(false);
  world->setConstraintForceMixingEnabled(false);

  SkeletonPtr jumpworm = Skeleton::create("jumpworm");

  std::pair<TranslationalJoint2D*, BodyNode*> rootJointPair
      = jumpworm->createJointAndBodyNodePair<TranslationalJoint2D>(nullptr);
  TranslationalJoint2D* rootJoint = rootJointPair.first;
  BodyNode* root = rootJointPair.second;

  std::shared_ptr<BoxShape> shape(new BoxShape(Eigen::Vector3d(0.1, 0.1, 0.1)));
  ShapeNode* rootVisual
      = root->createShapeNodeWith<VisualAspect, CollisionAspect>(shape);
  Eigen::Vector3d black = Eigen::Vector3d::Zero();
  rootVisual->getVisualAspect()->setColor(black);
  rootJoint->setForceUpperLimit(0, 0);
  rootJoint->setForceLowerLimit(0, 0);
  rootJoint->setForceUpperLimit(1, 0);
  rootJoint->setForceLowerLimit(1, 0);
  rootJoint->setVelocityUpperLimit(0, 1000.0);
  rootJoint->setVelocityLowerLimit(0, -1000.0);
  rootJoint->setVelocityUpperLimit(1, 1000.0);
  rootJoint->setVelocityLowerLimit(1, -1000.0);
  rootJoint->setPositionUpperLimit(0, 5);
  rootJoint->setPositionLowerLimit(0, -5);
  rootJoint->setPositionUpperLimit(1, 5);
  rootJoint->setPositionLowerLimit(1, -5);

  BodyNode* tail1 = createTailSegment(
      root, Eigen::Vector3d(182.0 / 255, 223.0 / 255, 144.0 / 255));
  BodyNode* tail2 = createTailSegment(
      tail1, Eigen::Vector3d(223.0 / 255, 228.0 / 255, 163.0 / 255));
  BodyNode* tail3 = createTailSegment(
      tail2, Eigen::Vector3d(221.0 / 255, 193.0 / 255, 121.0 / 255));

  Eigen::VectorXd pos = Eigen::VectorXd(5);
  pos << 0, 0, 90, 90, 45;
  jumpworm->setPositions(pos * 3.1415 / 180);

  world->addSkeleton(jumpworm);

  // Floor

  SkeletonPtr floor = Skeleton::create("floor");

  std::pair<WeldJoint*, BodyNode*> floorJointPair
      = floor->createJointAndBodyNodePair<WeldJoint>(nullptr);
  WeldJoint* floorJoint = floorJointPair.first;
  BodyNode* floorBody = floorJointPair.second;
  Eigen::Isometry3d floorOffset = Eigen::Isometry3d::Identity();
  floorOffset.translation() = Eigen::Vector3d(0, offGround ? -0.7 : -0.56, 0);
  floorJoint->setTransformFromParentBodyNode(floorOffset);
  std::shared_ptr<BoxShape> floorShape(
      new BoxShape(Eigen::Vector3d(2.5, 0.25, 0.5)));
  ShapeNode* floorVisual
      = floorBody->createShapeNodeWith<VisualAspect, CollisionAspect>(
          floorShape);
  floorBody->setFrictionCoeff(0);

  world->addSkeleton(floor);

  rootJoint->setVelocity(1, -0.1);
  Eigen::VectorXd vels = world->getVelocities();

  TrajectoryLossFn loss = [](const TrajectoryRollout* rollout) {
    const Eigen::Ref<const Eigen::MatrixXd> poses
        = rollout->getPosesConst("identity");
    const Eigen::Ref<const Eigen::MatrixXd> vels
        = rollout->getVelsConst("identity");
    const Eigen::Ref<const Eigen::MatrixXd> forces
        = rollout->getForcesConst("identity");

    double maxPos = -1000;
    double minPos = 1000;
    for (int i = 0; i < poses.cols(); i++)
    {
      if (poses(1, i) > maxPos)
      {
        maxPos = poses(1, i);
      }
      if (poses(1, i) < minPos)
      {
        minPos = poses(1, i);
      }
    }
    double peakPosLoss = -(maxPos * maxPos) * (maxPos > 0 ? 1.0 : -1.0);
    double minPosLoss = -(minPos * minPos) * (minPos > 0 ? 1.0 : -1.0);
    double endPos = poses(1, poses.cols() - 1);
    double endPosLoss = -(endPos * endPos) * (endPos > 0 ? 1.0 : -1.0);

    double forceLoss = forces.squaredNorm();

    // return endPosLoss * 100 + forceLoss * 1e-3;
    // return forceLoss;
    return endPosLoss; // + forceLoss;
    // return (100 * peakPosLoss) + (20 * minPosLoss) + endPosLoss;
  };

  TrajectoryLossFnAndGrad lossGrad = [](const TrajectoryRollout* rollout,
                                        TrajectoryRollout* gradWrtRollout // OUT
                                     ) {
    gradWrtRollout->getPoses("identity").setZero();
    gradWrtRollout->getVels("identity").setZero();
    gradWrtRollout->getForces("identity").setZero();
    const Eigen::Ref<const Eigen::MatrixXd> poses
        = rollout->getPosesConst("identity");
    const Eigen::Ref<const Eigen::MatrixXd> vels
        = rollout->getVelsConst("identity");
    const Eigen::Ref<const Eigen::MatrixXd> forces
        = rollout->getForcesConst("identity");

    gradWrtRollout->getPoses("identity")(1, poses.cols() - 1)
        = 2 * poses(1, poses.cols() - 1);
    double endPos = poses(1, poses.cols() - 1);
    double endPosLoss = -(endPos * endPos) * (endPos > 0 ? 1.0 : -1.0);
    return endPosLoss;
  };

  // Make a huge timestep, to try to make the gradients easier to get exactly
  // for finite differencing
  world->setTimeStep(1e-3);

  world->setPenetrationCorrectionEnabled(false);
  world->setConstraintForceMixingEnabled(false);

  // EXPECT_TRUE(verifyVelGradients(world, world->getVelocities()));
  // EXPECT_TRUE(verifyNoMultistepIntereference(world, 10));
  // EXPECT_TRUE(verifyAnalyticalJacobians(world));
  // EXPECT_TRUE(verifyAnalyticalBackprop(world));

  LossFn lossFn(loss);
  MultiShot shot(world, lossFn, 100, 20, false);
  std::shared_ptr<IKMapping> ikMap = std::make_shared<IKMapping>(world);
  ikMap->addLinearBodyNode(root);
  shot.addMapping("ik", ikMap);

  IPOptOptimizer optimizer = IPOptOptimizer();

  optimizer.setIterationLimit(5);
  optimizer.setSuppressOutput(true);
  optimizer.setRecoverBest(false);
  std::shared_ptr<Solution> record = optimizer.optimize(&shot);
  EXPECT_TRUE(record->getNumSteps() == 2);
  EXPECT_TRUE(record->getStep(0).index == 0);
  EXPECT_TRUE(record->getStep(1).index == 1);
  EXPECT_TRUE(record->getStep(1).rollout != record->getStep(0).rollout);

  for (int i = 0; i < 10; i++)
  {
    std::cout << "Step " << i << std::endl;
    record->reoptimize();
  }
}
// #endif