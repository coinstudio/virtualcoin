#include <boost/algorithm/string.hpp>
#include <boost/foreach.hpp>
#include <boost/test/unit_test.hpp>

#include "base58.h"
#include "util.h"
#include "main.h"

using namespace std;

BOOST_AUTO_TEST_SUITE(virtualsend_tests)

BOOST_AUTO_TEST_CASE(virtualsend_sign)
{

    std::string errorMessage = "";
    std::string strBase64;
    CKey key;
    CPubKey pubkey;
    std::vector<unsigned char> vchSig;

    CVirtualSendSigner dss;
    dss.SetKey("XDPugk3QgxVpQ4BubgzKaXhQudtaBnjuos9w6ZTojYx68EipNnt7", errorMessage, key, pubkey);
    BOOST_CHECK(dss.SignMessage("hello", errorMessage, vchSig, key) == true);
    BOOST_CHECK(dss.VerifyMessage(pubkey, vchSig, "hello", errorMessage) == true);
    BOOST_CHECK(dss.VerifyMessage(pubkey, vchSig, "hello2", errorMessage) == false);

}

BOOST_AUTO_TEST_CASE(set_collateral_address_bad)
{
	CVirtualSendPool * dsp_ptr = new CVirtualSendPool();

	string crappy = "badaddress";

	BOOST_CHECK( dsp_ptr->SetCollateralAddress(crappy) == false );
	delete dsp_ptr;
}

BOOST_AUTO_TEST_CASE(set_collateral_address_production)
{
	CVirtualSendPool * dsp_ptr = new CVirtualSendPool();

	string prod = "Xq19GqFvajRrEdDHYRKGYjTsQfpV5jyipF";

	BOOST_CHECK( dsp_ptr->SetCollateralAddress(prod) == true );
	delete dsp_ptr;
}

BOOST_AUTO_TEST_CASE(set_collateral_address_testnet)
{
	CVirtualSendPool * dsp_ptr = new CVirtualSendPool();

	string testnet = "mxE2Rp3oYpSEFdsN5TdHWhZvEHm3PJQQVm";

	BOOST_CHECK( dsp_ptr->SetCollateralAddress(testnet) == true );
	delete dsp_ptr;
}


BOOST_AUTO_TEST_CASE(virtualsend_vote)
{
    CPubKey key;
    CMasterNodeVote mnv;
    mnv.Set(key, 1);
    mnv.Vote();
    BOOST_CHECK(mnv.GetVotes() == 2);
    mnv.Vote();
    BOOST_CHECK(mnv.GetVotes() == 3);
}


BOOST_AUTO_TEST_SUITE_END()
