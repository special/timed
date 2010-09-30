#include <cstring>

#define F_AEGIS_LIBCREDS 1 // XXX: make a f.h file and include it here

#if F_AEGIS_LIBCREDS
#include <sys/creds.h>
#endif

#include <qm/log>
#include <iodata/iodata>

#include "timed/interface"
#include "credentials.h"

#include "creds.h"


#if F_AEGIS_LIBCREDS // XXX: this is a wrong feature name
credentials_t credentials_t::from_aegis_creds(creds_t aegis_creds)
{
  credentials_t creds ; // uid/gid is set to nobody/nogroup by default

  // using a buffer directly on stack, if not enough -> use heap
  static const size_t buf_stack_len = 1024 ;
  char buf_on_stack[buf_stack_len] ;

  char *buf = buf_on_stack ;
  size_t len = buf_stack_len ;

  creds_type_t aegis_type ;
  creds_value_t aegis_val ;
  for(int i=0; (aegis_type = creds_list(aegis_creds, i,  &aegis_val)) != CREDS_BAD; ++i)
  {
    int res1 = creds_creds2str(aegis_type, aegis_val, buf, len) ;
    bool error1 = res1 < 0 ;
    bool memory1 = len <= (size_t)res1 ;
    bool failure1 = error1 || memory1 ;

    if (failure1 && buf!=buf_on_stack)
      delete [] buf ;

    if (error1)
    {
      log_error("creds_creds2str() failed (%d returned)", res1) ;
      return credentials_t() ; // new empty object, being paranoid
    }

    if (memory1) // have to re-alloc
    {
      len = (size_t)res1 + 1, buf = new char[len] ;
      int res2 = creds_creds2str(aegis_type, aegis_val, buf, len) ;
      bool failure2 = res2 < 0 || (len <= (size_t)res2) ;
      if(failure2)
      {
        log_error("creds_creds2str() failed second time (len=%u, returned value=%d)", len, res2) ;
        delete [] buf ;
        return credentials_t() ;
      }
    }

    // now 'buf' is containing a proper '\0'-terminated c-string.

    bool is_token = true ;
    static const char *id[] = {"UID::", "GID::"} ; // loop over 'uid' and 'gid'
    static const int prefix_len = 5 ;

    for (int j=0; j<2; ++j)
      if (strncmp(buf, id[j], prefix_len)==0) // found!
      {
        is_token = false ;
        const char *shifted = buf + prefix_len ;
        if(*shifted == '\0')
        {
          log_warning("got empty uid/gid credential value in '%s'", buf) ;
          break ;
        }

        (j==0 ? creds.uid : creds.gid) = (string) shifted ;
        break ;
      }

    if(is_token)
      creds.tokens.insert(buf) ;
  }

  if (buf!=buf_on_stack)
    delete [] buf ;

  return creds ;
}

bool credentials_t::aegis_add_string_to_creds(creds_t &aegis_creds, const string &token, bool silent)
{
  creds_value_t aegis_val ;
  creds_type_t aegis_type = creds_str2creds(token.c_str(), &aegis_val) ;
  if (aegis_type == CREDS_BAD || aegis_val == CREDS_BAD)
  {
    if (!silent)
      log_error("failed to recognize aegis token '%s': bad %s", token.c_str(), aegis_type == CREDS_BAD ? "type" : "value") ;
    return false ;
  }

  if (creds_add(&aegis_creds, aegis_type, aegis_val) == -1)
  {
    if (!silent)
      log_error("aegis creds_add() failed for token '%s'", token.c_str()) ;
    return false ;
  }

  return true ;
}

creds_t credentials_t::to_aegis_creds() const
{
  creds_t aegis_creds = creds_init() ;

  bool ok = true ;
  for(set<string>::const_iterator it=tokens.begin(); it!=tokens.end() && ok; ++it)
    ok = aegis_add_string_to_creds(aegis_creds, *it, false) ;
  ok = ok && aegis_add_string_to_creds(aegis_creds, (string)"UID::" + uid, false) ;
  ok = ok && aegis_add_string_to_creds(aegis_creds, (string)"GID::" + gid, false) ;

  if (!ok)
    creds_free(aegis_creds), aegis_creds = creds_init() ;

  return aegis_creds ;
}

// TODO: F_CRED_UID
//       implement the same function without aegis, asking UID of the caller and
//       setting this UID and the caller's default GID as only available credentials

// TODO: F_CRED_NOBODY
//       implement the same function setting nobody/nogroup as credentials

// TODO: F_CRED_AEGIS --- make this function #ifdef'ed
credentials_t credentials_t::from_dbus_connection(const QDBusMessage &message)
{
  // We are doing this in a kinda insecure way. Two steps:
  // 1. Ask dbus daemon, what is the pid of the client.
  // --- race race race --- (please someone file a bug about it) --- race race race ---
  // 2. Ask aegis kernel extension, what are the credentials of given pid.

  QString sender = message.service() ;
  /* "returns "sender" on inbound messages
      and "service" on outbound messages
      which saves one QString object and
      confuses at least me ..." -- so true ! */

  // 1. Ask DBus daemon, what is the PID of the 'sender':

  pid_t pid = credentials_get_name_owner(Maemo::Timed::bus(), sender) ;

  if (pid < 0)
  {
    log_warning("can't get pid of the caller, already terminated?") ;
    return credentials_t() ;
  }

  // 2. Getting aegis credentials from the kernel, by pid

  creds_t aegis_creds = creds_gettask(pid) ;

  // Don't check result, as NULL is a valid set of aegis credentials

  credentials_t creds = credentials_t::from_aegis_creds(aegis_creds) ;

  creds_free(aegis_creds) ;

  return creds ;
}

#endif // F_AEGIS_LIBCREDS

bool credentials_t::apply() const
{
#if 1 // F_CREDS_AEGIS
  creds_t aegis_creds_want = to_aegis_creds() ;

  bool res = creds_set(aegis_creds_want) == 0 ;

  if (!res)
    log_error("aegis cred_set() failed") ;

  creds_free(aegis_creds_want) ;

  return res ;
#else // F_CREDS_AEGIS
#error credentials_t::apply() is only implemented for F_CREDS_AEGIS
#endif
}

void credentials_t::from_current_process()
{
#if 1 // F_CREDS_AEGIS
  creds_t aegis_creds = creds_gettask(0) ;
  *this = credentials_t::from_aegis_creds(aegis_creds) ;

  creds_free(aegis_creds) ;
#else // F_CREDS_AEGIS
#error credentials_t::apply() is only implemented for F_CREDS_AEGIS
#endif
}

bool credentials_t::apply_and_compare()
{
  if (! apply()) // can't apply: nothing to check
    return false ;

  credentials_t current ;
  current.from_current_process() ;

  ostringstream os ;

  bool equal = true ;
#define COMMA (os << (equal ? "" : ", ") )

  if (current.uid != uid)
    COMMA << "current uid='" << current.uid << "' (requested uid='" << uid <<"')", equal = false ;

  if (current.gid != gid)
    COMMA << "current gid='" << current.gid << "' (requested gid='" << gid <<"')", equal = false ;

  int all_accrued = true ;
#define COMMA_A (all_accrued ? COMMA << "tokens not present: {" : os << ", ")

  for (set<string>::const_iterator it=tokens.begin(); it!=tokens.end(); ++it)
    if (current.tokens.count(*it)==0)
      COMMA_A << "'" << *it << "'", all_accrued = false ;
  if (!all_accrued)
    os << "}" ;

  equal = equal && all_accrued ;

  int all_dropped = true ;
#define COMMA_D (all_dropped ? COMMA << "tokens not dropped: {" : os << ", ")

  for (set<string>::const_iterator it=current.tokens.begin(); it!=current.tokens.end(); ++it)
    if (tokens.count(*it)==0)
      COMMA_D << "'" << *it << "'", all_accrued = false ;
  if (!all_dropped)
    os << "}" ;

  equal = equal && all_dropped ;

  if(!equal)
    log_warning("applied and wanted credentials differ: %s", os.str().c_str()) ;

  return equal ;

#undef COMMA
#undef COMMA_A
#undef COMMA_D
}


iodata::record *credentials_t::save() const
{
  iodata::record *r = new iodata::record ;
  r->add("uid", uid) ;
  r->add("gid", gid) ;
  iodata::array *tok = new iodata::array ;
  for(set<string>::const_iterator it=tokens.begin(); it!=tokens.end(); ++it)
    tok->add(new iodata::bytes(*it)) ;
  r->add("tokens", tok) ;

  return r ;
}

void credentials_t::load(const iodata::record *r)
{
  uid = r->get("uid")->str() ;
  gid = r->get("gid")->str() ;
  const iodata::array *tok = r->get("tokens")->arr() ;
  for(unsigned i=0; i<tok->size(); ++i)
    tokens.insert(tok->get(i)->str()) ;
}