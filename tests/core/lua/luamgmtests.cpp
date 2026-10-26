/*******************************************************************************
 * Copyright (c) 2026 Aimirim STI
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0.
 *
 * SPDX-License-Identifier: EPL-2.0
 *
 * Contributors:
 *   Pedro Ricardo C. Souza - initial implementation
 *******************************************************************************/

#include <boost/test/unit_test.hpp>

#include <string>

#include "../../../stdfblib/system/src/CommandParser.h"
#include "forte/mgmcmdstruct.h"
#include "forte/util/string_utils.h"

#include "forte_boost_output_support.h"
#include "../fbtests/fbtesterglobalfixture.h"

using namespace forte::literals;
using forte::test::CFBTestDataGlobalFixture;

namespace forte::lua::test {

  namespace {
    constexpr char cAdapter[] = R"lua(
return {interfaceSpec = {
  numEIs = 1, EINames = {"REQ"}, EIWith = {0, 65535}, EIWithIndexes = {0},
  numEOs = 1, EONames = {"CNF"}, EOWith = {0, 65535}, EOWithIndexes = {0},
  numDIs = 1, DINames = {"IN"}, DIDataTypeNames = {"INT"},
  numDOs = 1, DONames = {"OUT"}, DODataTypeNames = {"INT"}
}}
)lua";

    // REQ copies IN to OUT plus one and fires CNF, the comparison needs '<' and '&' to be escaped
    constexpr char cIncrement[] = R"lua(
local text = "a < b & 'c'"
return {
  ECC = function(fb, id)
    if id == 0 and #text > 0 then
      fb[67108864] = fb[33554432] + 1
      fb(0)
    end
  end,
  interfaceSpec = {
    numEIs = 1, EINames = {"REQ"}, EIWith = {0, 65535}, EIWithIndexes = {0},
    numEOs = 1, EONames = {"CNF"}, EOWith = {0, 65535}, EOWithIndexes = {0},
    numDIs = 1, DINames = {"IN"}, DIDataTypeNames = {"INT"},
    numDOs = 1, DONames = {"OUT"}, DODataTypeNames = {"INT"},
    numAdapters = 1,
    adapterInstanceDefinition = {{adapterNameID = "P", adapterTypeNameID = "luamgmtest::AValue", isPlug = true}}
  }
}
)lua";

    constexpr char cWrapper[] = R"lua(
return {
  interfaceSpec = {
    numEIs = 1, EINames = {"REQ"}, EIWith = {0, 65535}, EIWithIndexes = {0},
    numEOs = 1, EONames = {"CNF"}, EOWith = {0, 65535}, EOWithIndexes = {0},
    numDIs = 1, DINames = {"IN"}, DIDataTypeNames = {"INT"},
    numDOs = 1, DONames = {"OUT"}, DODataTypeNames = {"INT"},
    numAdapters = 0, adapterInstanceDefinition = {}
  },
  fbnSpec = {
    internalFBs = {{fbNameID = "INC", fbTypeID = "luamgmtest::Increment"}}, parameters = {},
    eventConnections = {
      {srcID = "REQ", srcFBNum = -1, dstID = "REQ", dstFBNum = 0},
      {srcID = "CNF", srcFBNum = 0, dstID = "CNF", dstFBNum = -1}
    },
    dataConnections = {
      {srcID = "IN", srcFBNum = -1, dstID = "IN", dstFBNum = 0},
      {srcID = "OUT", srcFBNum = 0, dstID = "OUT", dstFBNum = -1}
    },
    adapterConnections = {},
    numFBs = 1, numECons = 2, numDCons = 2, numAdpCons = 0, numParams = 0
  }
}
)lua";

    const std::string cFBInterface =
        "interfaceSpec = {numEIs = 0, EINames = {}, EIWith = {}, EIWithIndexes = {}, numEOs = 0, EONames = {}, "
        "EOWith = {}, EOWithIndexes = {}, numDIs = 1, DINames = {\"IN\"}, DIDataTypeNames = {\"%s\"}, numDOs = 0, "
        "DONames = {}, DODataTypeNames = {}, numAdapters = 0, adapterInstanceDefinition = {}}";

    std::string basicFB(std::string_view paDataType, std::string_view paBody = "") {
      std::string interface = cFBInterface;
      interface.replace(interface.find("%s"), 2, paDataType);
      return "return {ECC = function(fb, id) " + std::string(paBody) + " end, " + interface + "}";
    }

    std::string escape(std::string_view paText) {
      std::string escaped(paText);
      util::transformNonEscapedToEscapedXMLText(escaped, 0);
      return escaped;
    }

    std::string createRequest(std::string_view paElement, std::string_view paName, std::string_view paSource) {
      return "<Request ID=\"3\" Action=\"CREATE\"><" + std::string(paElement) + " Name=\"" + std::string(paName) +
             "\">" + escape(paSource) + "</" + std::string(paElement) + "></Request>";
    }

    //! A resource of its own, with the request path of the device manager: parse, execute, respond
    class CManagedResource {
      public:
        explicit CManagedResource(StringId paName) : mResource(paName, CFBTestDataGlobalFixture::getDevice()) {
          BOOST_REQUIRE(mResource->initialize());
        }

        ~CManagedResource() {
          mResource->deinitialize();
        }

        EMGMResponse execute(const std::string &paRequest, std::string &paResponseText) {
          SManagementCMD command;
          iec61499::system::CommandParser parser(command);
          EMGMResponse response = parser.parseMGMCommand("", paRequest);
          if (response == EMGMResponse::Ready) {
            response = mResource->executeMGMCommand(command);
          }
          CIEC_STRING text;
          parser.generateResponse(text, response);
          paResponseText = text.getStorage();
          return response;
        }

        EMGMResponse execute(const std::string &paRequest) {
          std::string responseText;
          return execute(paRequest, responseText);
        }

        CResource &operator*() {
          return *mResource;
        }

      private:
        CInternalFB<iec61499::system::EMB_RES> mResource;
    };
  } // namespace

  BOOST_AUTO_TEST_SUITE(LuaMGMTests)

  BOOST_AUTO_TEST_CASE(createTypesAndUseThem) {
    CManagedResource resource("LuaMGMTest_create"_STRID);
    std::string responseText;

    BOOST_TEST(resource.execute(createRequest("AdapterType", "luamgmtest::AValue", cAdapter), responseText) ==
               EMGMResponse::Ready);
    BOOST_TEST(responseText == "<Response ID=\"3\" />");
    BOOST_TEST(resource.execute(createRequest("FBType", "luamgmtest::Increment", cIncrement), responseText) ==
               EMGMResponse::Ready);
    BOOST_TEST(responseText == "<Response ID=\"3\" />");
    BOOST_TEST(resource.execute(createRequest("FBType", "luamgmtest::Wrapper", cWrapper)) == EMGMResponse::Ready);

    BOOST_TEST(resource.execute(R"(<Request ID="4" Action="QUERY"><FBType Name="*" /></Request>)", responseText) ==
               EMGMResponse::Ready);
    BOOST_TEST(responseText.find("luamgmtest::Increment") != std::string::npos);
    BOOST_TEST(responseText.find("luamgmtest::Wrapper") != std::string::npos);
    BOOST_TEST(resource.execute(R"(<Request ID="5" Action="QUERY"><AdapterType Name="*" /></Request>)", responseText) ==
               EMGMResponse::Ready);
    BOOST_TEST(responseText.find("luamgmtest::AValue") != std::string::npos);

    BOOST_TEST(
        resource.execute(R"(<Request ID="6" Action="CREATE"><FB Name="W" Type="luamgmtest::Wrapper" /></Request>)") ==
        EMGMResponse::Ready);
    BOOST_TEST(
        resource.execute(R"(<Request ID="7" Action="WRITE"><Connection Source="41" Destination="W.IN" /></Request>)") ==
        EMGMResponse::Ready);
    BOOST_TEST(
        resource.execute(R"(<Request ID="8" Action="READ"><Connection Source="W.IN" Destination="*" /></Request>)",
                         responseText) == EMGMResponse::Ready);
    BOOST_TEST(responseText.find("Destination=\"41\"") != std::string::npos);

    // the same definition again changes nothing
    BOOST_TEST(resource.execute(createRequest("FBType", "luamgmtest::Wrapper", cWrapper)) == EMGMResponse::Ready);
    // another definition while W exists
    BOOST_TEST(resource.execute(createRequest("FBType", "luamgmtest::Wrapper", basicFB("INT")), responseText) ==
               EMGMResponse::InvalidState);
    BOOST_TEST(responseText == "<Response ID=\"3\" Reason=\"INVALID_STATE\" />");

    BOOST_TEST(resource.execute(R"(<Request ID="9" Action="DELETE"><FB Name="W" Type="*" /></Request>)") ==
               EMGMResponse::Ready);
    BOOST_TEST(resource.execute(createRequest("FBType", "luamgmtest::Wrapper", basicFB("INT"))) == EMGMResponse::Ready);
  }

  BOOST_AUTO_TEST_CASE(reasons) {
    CManagedResource resource("LuaMGMTest_reasons"_STRID);
    std::string responseText;
    const auto createFB = [&resource, &responseText](std::string_view paName, std::string_view paSource) {
      return resource.execute(createRequest("FBType", paName, paSource), responseText);
    };

    BOOST_TEST(createFB("luamgmtest::Syntax", "return {") == EMGMResponse::InvalidObject);
    BOOST_TEST(responseText == "<Response ID=\"3\" Reason=\"INVALID_OBJECT\" />");
    BOOST_TEST(createFB("luamgmtest::Runtime", "error('at load time')") == EMGMResponse::InvalidObject);
    BOOST_TEST(createFB("luamgmtest::NoTable", "return 1") == EMGMResponse::InvalidObject);
    BOOST_TEST(createFB("luamgmtest::Schema", "return {ECC = function() end}") == EMGMResponse::BadParams);
    BOOST_TEST(createFB("luamgmtest::Neither", "return {}") == EMGMResponse::BadParams);
    BOOST_TEST(createFB("luamgmtest::DataType", basicFB("luamgmtest::NoSuchStruct")) == EMGMResponse::UnsupportedType);
    BOOST_TEST(responseText == "<Response ID=\"3\" Reason=\"UNSUPPORTED_TYPE\" />");
    BOOST_TEST(createFB("iec61499::events::E_CTU", basicFB("INT")) == EMGMResponse::InvalidOperation);
    BOOST_TEST(createFB("not a name", basicFB("INT")) == EMGMResponse::BadParams);
    BOOST_TEST(createFB("luamgmtest::Fine", basicFB("INT")) == EMGMResponse::Ready);
    BOOST_TEST(resource.execute(createRequest("AdapterType", "luamgmtest::BadAdapter", "return {}")) ==
               EMGMResponse::BadParams);
  }

  BOOST_AUTO_TEST_CASE(runawayAndHostileModules) {
    CManagedResource resource("LuaMGMTest_hostile"_STRID);
    const auto create = [&resource](std::string_view paElement, std::string_view paName, std::string_view paSource) {
      return resource.execute(createRequest(paElement, paName, paSource));
    };

    // the instruction budget of CREATE stops modules that do not finish, even when they catch errors
    BOOST_TEST(create("FBType", "luamgmtest::Endless", "while true do end") == EMGMResponse::InvalidObject);
    BOOST_TEST(create("FBType", "luamgmtest::Swallow", "while true do pcall(function() while true do end end) end") ==
               EMGMResponse::InvalidObject);
    BOOST_TEST(create("AdapterType", "luamgmtest::EndlessAdapter", "while true do end") == EMGMResponse::InvalidObject);
    BOOST_TEST(create("FBType", "luamgmtest::Recursion", "local function f() return f() + 1 end return f()") ==
               EMGMResponse::InvalidObject);

    // the tables of a module are read raw, metamethods never run
    BOOST_TEST(
        create("FBType", "luamgmtest::Trap", "return setmetatable({}, {__index = function() while true do end end})") ==
        EMGMResponse::BadParams);
    std::string trappedInterface = basicFB("INT");
    trappedInterface.replace(trappedInterface.find("interfaceSpec = {"), 17,
                             "interfaceSpec = setmetatable({}, {__index = function() error('trap') end}), x = {");
    BOOST_TEST(create("FBType", "luamgmtest::TrappedInterface", trappedInterface) == EMGMResponse::BadParams);
    BOOST_TEST(create("FBType", "luamgmtest::TrappedNetwork",
                      "return {fbnSpec = setmetatable({}, {__index = function() error('trap') end}), " +
                          basicFB("INT").substr(basicFB("INT").find("interfaceSpec"))) == EMGMResponse::BadParams);

    // nothing of the refused modules was registered
    std::string responseText;
    BOOST_TEST(resource.execute(R"(<Request ID="4" Action="QUERY"><FBType Name="*" /></Request>)", responseText) ==
               EMGMResponse::Ready);
    BOOST_TEST(responseText.find("luamgmtest::Endless") == std::string::npos);
    BOOST_TEST(responseText.find("luamgmtest::Trap") == std::string::npos);

    // a module that loops in its ECC only is created, the budget applies to the module's top level
    BOOST_TEST(create("FBType", "luamgmtest::LoopsInECC", basicFB("INT", "while true do end")) == EMGMResponse::Ready);
  }

  BOOST_AUTO_TEST_SUITE_END()

} // namespace forte::lua::test
