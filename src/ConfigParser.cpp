/**
 * Copyright (c) 2013-2017, The PurpleI2P Project
 * Copyright (c) 2019-2021 polistern
 *
 * This file is part of Purple i2pd project and licensed under BSD3
 *
 * See full license text in LICENSE file at top of project tree
 */

#include <boost/program_options/cmdline.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

#include "ConfigParser.h"
#include "version.h"

using namespace boost::program_options;

namespace pbote {
namespace config {

template<typename T>
boost::program_options::typed_value<T> *make_value(T *store_to) {
  return boost::program_options::value<T>(store_to);
}

options_description m_OptionsDesc;
variables_map m_Options;
//std::vector<std::string> bootstrap_addresses;

void Init() {
  options_description general("General options");
  general.add_options()
      ("help", "Show this message")
      ("version", "Show pboted version")
      ("conf",
       value<std::string>()->default_value(""),
       "Path to main pboted config file (default: try ~/.pboted/pboted.conf or /var/lib/pboted/pboted.conf)")
      ("pidfile",
       value<std::string>()->default_value(""),
       "Path to pidfile (default: ~/pboted/pboted.pid or /var/lib/pboted/pbote.pid)")
      ("log", value<std::string>()->default_value("file"), "Logs destination: stdout, file, syslog (file if not set)")
      ("logfile", value<std::string>()->default_value(""), "Path to logfile (stdout if not set, autodetect if daemon)")
      ("loglevel",
       value<std::string>()->default_value("info"),
       "Set the minimal level of log messages (debug, info, warn, error, none)")
      ("logclftime",
       bool_switch()->default_value(false),
       "Write full CLF-formatted date and time to log (default: disabled, write only time)")
      ("datadir",
       value<std::string>()->default_value(""),
       "Path to storage of pboted data (keys, peer, packets, etc.) (default: try ~/.pboted/ or /var/lib/pboted/)")
      ("host", value<std::string>()->default_value("0.0.0.0"), "External IP")
      ("port", value<uint16_t>()->default_value(5050), "Port to listen for incoming connections (default: auto)")
      ("daemon", bool_switch()->default_value(false), "Router will go to background after start (default: disabled)")
      ("service",
       bool_switch()->default_value(false),
       "Service will use system folders like '/var/lib/pboted' (default: disabled)");
  options_description sam("SAM options");
  sam.add_options()
      ("sam.name", value<std::string>()->default_value("pbote"), "What name we send to I2P router (default: disabled)")
      ("sam.address", value<std::string>()->default_value("127.0.0.1"), "I2P SAM address (default: 127.0.0.1)")
      ("sam.tcp", value<uint16_t>()->default_value(7656), "I2P SAM port (default: 7656)")
      ("sam.udp", value<uint16_t>()->default_value(7655), "I2P SAM port (default: 7655)")
    /*("sam.auth", bool_switch()->default_value(false),                 "If SAM authentication requered (default: disabled)")
    ("sam.login", value<std::string>()->default_value(""),            "SAM login")
    ("sam.password", value<std::string>()->default_value(""),         "SAM password")*/
      ;
  options_description bootstrap("Bootstrap options");
  bootstrap.add_options()
      //("bootstrap.address", make_value(&bootstrap_addresses), "516-byte I2P destination key in Base64 format")
      ("bootstrap.address", value<std::vector<std::string>>(), "516-byte I2P destination key in Base64 format");
  /*options_description mail("Mail options");
  mail.add_options()
  ("mail.autocheck", bool_switch()->default_value(true),       "Allow auto mail check (default: enabled)")
  ("mail.checkinterval", value<uint16_t>()->default_value(30), "Auto mail check interval in minutes (default: 30)")
  ("mail.deliverycheck", bool_switch()->default_value(true),   "Allow to check mail delivery (default: enabled)")
  ("mail.hidelocale", bool_switch()->default_value(true),      "Allow to hide system locale (default: enabled)")
  ;
  options_description delivery("Delivery options");
  delivery.add_options()
  ("delivery.hops", value<uint8_t>()->default_value(3),      "Count of hops for mail sending (default: 3)")
  ("delivery.delay", bool_switch()->default_value(true),     "Use delay on relay for mail sending (default: enabled)")
  ("delivery.delaymin", value<uint8_t>()->default_value(5),  "Minimum delay for mail sending in minutes(default: 5)")
  ("delivery.delaymax", value<uint8_t>()->default_value(15), "Maximum delay for mail sending in minutes(default: 15)")
  ;
  options_description smtp("SMTP options");
  smtp.add_options()
  ("smtp.enabled", bool_switch()->default_value(false), "Allow connect via SMTP (default: disabled)")
  ("smtp.port", value<uint16_t>()->default_value(25),   "SMTP listen port (default: 25)")
  ;
  options_description imap("IMAP options");
  imap.add_options()
  ("imap.enabled", bool_switch()->default_value(false), "Allow connect via IMAP (default: disabled)")
  ("imap.port", value<uint16_t>()->default_value(143),  "IMAP listen port (default: 143)")
  ;
  options_description pop3("POP3 options");
  pop3.add_options()
  ("pop3.enabled", bool_switch()->default_value(false), "Allow connect via POP3 (default: disabled)")
  ("pop3.port", value<uint16_t>()->default_value(110),  "POP3 listen port (default: 110)")
  ;
  options_description clearnet("Clearnet proxy options");
  clearnet.add_options()
  ("clearnet.enable", bool_switch()->default_value(false),          "Use node for sending mail via clearnet (default: disabled)")
  ("clearnet.destination", value<std::string>()->default_value(""), "Node addresses for sending mail via clearnet")
  ;*/
  m_OptionsDesc
      .add(general)
      .add(sam)
      .add(bootstrap)
    /*.add(mail)
    .add(delivery)
    .add(smtp)
    .add(imap)
    .add(pop3)
    .add(clearnet)*/
      ;
}

void ParseCmdline(int argc, char *argv[], bool ignoreUnknown) {
  try {
    auto style = command_line_style::unix_style | command_line_style::allow_long_disguise;
    style &= ~command_line_style::allow_guessing;
    if (ignoreUnknown)
      store(command_line_parser(argc, argv).options(m_OptionsDesc).style(style).allow_unregistered().run(), m_Options);
    else
      store(parse_command_line(argc, argv, m_OptionsDesc, style), m_Options);
  } catch (boost::program_options::error &e) {
    std::cerr << "args: " << e.what() << std::endl;
    exit(EXIT_FAILURE);
  }

  if (!ignoreUnknown && (m_Options.count("help") || m_Options.count("h"))) {
    std::cout << "pboted version " << PBOTE_VERSION << " (" << PBOTE_VERSION << ")" << std::endl;
    std::cout << m_OptionsDesc;
    exit(EXIT_SUCCESS);
  } else if (m_Options.count("version")) {
    std::cout << "pboted version " << PBOTE_VERSION << " (" << PBOTE_VERSION << ")" << std::endl;
    std::cout << "Boost version " << BOOST_VERSION / 100000 << "." // maj.version
              << BOOST_VERSION / 100 % 1000 << "." // min.version
              << BOOST_VERSION % 100            // patch version
              << std::endl;
#if defined(OPENSSL_VERSION_TEXT)
    std::cout << OPENSSL_VERSION_TEXT << std::endl;
#endif
#if defined(LIBRESSL_VERSION_TEXT)
    std::cout << LIBRESSL_VERSION_TEXT << std::endl;
#endif
    exit(EXIT_SUCCESS);
  }
}

void ParseConfig(const std::string &path) {
  if (path.empty()) return;

  std::ifstream config(path, std::ios::in);

  if (!config.is_open()) {
    std::cerr << "missing/unreadable config file: " << path << std::endl;
    exit(EXIT_FAILURE);
  }

  try {
    store(boost::program_options::parse_config_file(config, m_OptionsDesc), m_Options);
  } catch (boost::program_options::error &e) {
    std::cerr << e.what() << std::endl;
    exit(EXIT_FAILURE);
  };
}

void Finalize() {
  notify(m_Options);
}

bool IsDefault(const char *name) {
  if (!m_Options.count(name))
    throw "try to check non-existent option";

  return m_Options[name].defaulted();
}

bool GetOptionAsAny(const char *name, boost::any &value) {
  if (!m_Options.count(name))
    return false;
  value = m_Options[name];
  return true;
}

bool GetOptionAsAny(const std::string &name, boost::any &value) {
  return GetOptionAsAny(name.c_str(), value);
}

} // namespace config
} // namespace pbote