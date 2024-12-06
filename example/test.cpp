#include "test.h"

char ASomething::s_memStatic = 'z';

int ASomething::SetInteger(int a_value, float& a_otherValue){
  int someLocal = 11;
  static float s_funcStatic = 1.23;
  m_integer = a_value + a_otherValue;
  m_float = m_integer + m_float + a_value;
  someLocal = m_integer + m_float;
  s_funcStatic = a_otherValue;
  m_noInit = m_integer * 2;
  return someLocal;
}
