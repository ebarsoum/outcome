#include "../../include/boost/outcome.hpp"

using namespace boost::outcome;
extern outcome<int> unknown();
extern BOOST_NOINLINE std::exception_ptr test1()
{
  return unknown().get_exception();
}
extern BOOST_NOINLINE void test2()
{
}

int main(void)
{
  int ret=0;
  if(std::make_exception_ptr(5)!=test1()) ret=1;
  test2();
  return ret;
}
