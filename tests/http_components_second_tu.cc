#include "../source/http/http.hpp"

bool OtherTranslationUnitWorks() {
    return Util::StatuDesc(200) == "OK" && _statu_msg.at(404) == "Not Found";
}
