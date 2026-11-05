#include <gtest/gtest.h>

#include <sstream>

#include <csipaussd/v11/sep.hxx>

#include "helper/documents.hpp"

using namespace csipaus::sep;
using namespace csipaus_test;

TEST(Controls, FollowsFunctionSetAssignmentsToDerPrograms) {
  auto fsaStream = fixture("function_set_assignments_list.xml");
  const auto fsaList = FunctionSetAssignmentsList_(fsaStream, parseFlags);

  EXPECT_EQ(fsaList->pollRate(), 3600u);
  ASSERT_EQ(fsaList->FunctionSetAssignments().size(), 1u);
  const auto &fsa = fsaList->FunctionSetAssignments().front();
  EXPECT_EQ(hexString(fsa.mRID()), "A1000000000000000000000000000001");
  ASSERT_TRUE(fsa.DERProgramListLink());
  EXPECT_EQ(fsa.DERProgramListLink()->href(), "/edev/3/fsa/1/derp");

  auto programStream = fixture("der_program_list.xml");
  const auto programs = DERProgramList_(programStream, parseFlags);

  EXPECT_EQ(programs->pollRate(), 300u);
  ASSERT_EQ(programs->DERProgram().size(), 1u);
  const auto &program = programs->DERProgram().front();
  EXPECT_EQ(program.primacy(), 2);
  ASSERT_TRUE(program.DERControlListLink());
  EXPECT_EQ(program.DERControlListLink()->href(), "/derp/1/derc");
  ASSERT_TRUE(program.DefaultDERControlLink());
  EXPECT_EQ(program.DefaultDERControlLink()->href(), "/derp/1/dderc");
  ASSERT_TRUE(program.ActiveDERControlListLink());
}

TEST(Controls, ReadsOperatingEnvelopeLimits) {
  auto stream = fixture("der_control_list.xml");
  const auto list = DERControlList_(stream, parseFlags);

  ASSERT_EQ(list->DERControl().size(), 2u);
  const auto &control = list->DERControl().front();
  EXPECT_EQ(hexString(control.mRID()), "C1000000000000000000000000000007");
  EXPECT_EQ(*control.replyTo(), "/rsps/1/rsp");
  EXPECT_EQ(hexString(control.responseRequired()), "03");
  EXPECT_EQ(control.creationTime(), 1789990000);
  EXPECT_EQ(control.EventStatus().currentStatus(), 1);
  EXPECT_FALSE(control.EventStatus().potentiallySuperseded());
  EXPECT_EQ(control.interval().start(), 1790000000);
  EXPECT_EQ(control.interval().duration(), 1800u);

  const auto &base = control.DERControlBase();
  ASSERT_TRUE(base.opModImpLimW());
  EXPECT_EQ(base.opModImpLimW()->multiplier(), 3);
  EXPECT_EQ(base.opModImpLimW()->value(), 10);
  ASSERT_TRUE(base.opModExpLimW());
  EXPECT_EQ(base.opModExpLimW()->multiplier(), 0);
  EXPECT_EQ(base.opModExpLimW()->value(), 1500);
  ASSERT_TRUE(base.opModGenLimW());
  EXPECT_EQ(base.opModGenLimW()->value(), 5000);
  ASSERT_TRUE(base.opModLoadLimW());
  EXPECT_EQ(base.opModLoadLimW()->value(), 7000);
  EXPECT_FALSE(base.opModConnect());
  EXPECT_FALSE(base.opModEnergize());
}

// EPRI's C client read a false boolean as true, so a disconnect looked like
// a connect.
TEST(Controls, ReadsADisconnectAsFalse) {
  auto stream = fixture("der_control_list.xml");
  const auto list = DERControlList_(stream, parseFlags);

  const auto &control = list->DERControl().back();
  EXPECT_EQ(control.EventStatus().currentStatus(), 0);
  EXPECT_TRUE(control.EventStatus().potentiallySuperseded());
  EXPECT_EQ(*control.EventStatus().potentiallySupersededTime(), 1789995000);
  EXPECT_EQ(*control.randomizeStart(), 60);

  const auto &base = control.DERControlBase();
  ASSERT_TRUE(base.opModConnect());
  EXPECT_FALSE(*base.opModConnect());
  ASSERT_TRUE(base.opModEnergize());
  EXPECT_FALSE(*base.opModEnergize());
  EXPECT_FALSE(base.opModExpLimW());
}

TEST(Controls, ReadsTheDefaultControl) {
  auto stream = fixture("default_der_control.xml");
  const auto control = DefaultDERControl_(stream, parseFlags);

  EXPECT_EQ(hexString(control->mRID()), "D1000000000000000000000000000001");
  const auto &base = control->DERControlBase();
  ASSERT_TRUE(base.opModEnergize());
  EXPECT_TRUE(*base.opModEnergize());
  ASSERT_TRUE(base.opModImpLimW());
  EXPECT_EQ(base.opModImpLimW()->value(), 15000);
  ASSERT_TRUE(base.opModExpLimW());
  EXPECT_EQ(base.opModExpLimW()->value(), 1500);
  EXPECT_EQ(*control->setGradW(), 27);
  EXPECT_EQ(*control->setSoftGradW(), 16);
}

TEST(Controls, WritesLimitsBackInTheCsipAusNamespace) {
  auto stream = fixture("der_control_list.xml");
  const auto list = DERControlList_(stream, parseFlags);

  std::ostringstream out;
  DERControlList_(out, *list, namespaces(), "UTF-8", serializeFlags);
  const std::string xml = out.str();
  EXPECT_NE(xml.find("xmlns=\"urn:ieee:std:2030.5:ns\""), std::string::npos);
  EXPECT_NE(xml.find("xmlns:csipaus=\"https://csipaus.org/ns\""),
            std::string::npos);
  EXPECT_NE(xml.find("<csipaus:opModExpLimW>"), std::string::npos);
  EXPECT_NE(xml.find("<opModEnergize>false</opModEnergize>"),
            std::string::npos);

  std::istringstream again(xml);
  const auto reparsed = DERControlList_(again, parseFlags);
  EXPECT_EQ(*reparsed, *list);
}

// Without validation, an element the schema does not define ends the
// parsing of the element it is in: what follows it there is skipped, with
// no error. The adapter has to guard against this, as it can drop a limit.
TEST(Controls, SkipsTheRestOfAnElementAfterOneItDoesNotKnow) {
  auto stream = fixture("default_der_control_vendor_element.xml");
  const auto control = DefaultDERControl_(stream, parseFlags);

  const auto &base = control->DERControlBase();
  ASSERT_TRUE(base.opModEnergize());
  EXPECT_FALSE(base.opModMaxLimW());
  EXPECT_FALSE(base.opModExpLimW());
  EXPECT_EQ(*control->setGradW(), 27);
}

// CSIP-AUS v1.3 moves its extensions to https://csipaus.org/ns/v1.3, so
// these bindings read a v1.3 server's limits as no limits at all.
TEST(Controls, DoesNotSeeCsipAusV13Limits) {
  auto stream = fixture("der_control_list_v13.xml");
  const auto list = DERControlList_(stream, parseFlags);

  ASSERT_EQ(list->DERControl().size(), 1u);
  const auto &base = list->DERControl().front().DERControlBase();
  EXPECT_FALSE(base.opModExpLimW());
  EXPECT_EQ(base, DERControlBase());
}
