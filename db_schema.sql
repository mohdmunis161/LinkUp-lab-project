DROP TABLE IF EXISTS deleted_shorthands;
DROP TABLE IF EXISTS shorthand_updates;
DROP TABLE IF EXISTS shorthands;
DROP TABLE IF EXISTS files;
DROP TABLE IF EXISTS messages;
DROP TABLE IF EXISTS contents;
DROP TABLE IF EXISTS users;

CREATE TABLE users (
  user_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  username VARCHAR(50) NOT NULL UNIQUE,
  email VARCHAR(255) NULL UNIQUE,
  password_hash VARCHAR(255) NOT NULL,
  display_name VARCHAR(100) NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  last_login_at TIMESTAMP NULL,
  is_active BOOLEAN NOT NULL DEFAULT TRUE,
  PRIMARY KEY (user_id),
  INDEX idx_username (username),
  INDEX idx_email (email)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE contents (
  content_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  user_id BIGINT UNSIGNED NULL,
  content_type ENUM('message','file','system') NOT NULL,
  direction ENUM('send','receive') NOT NULL,
  channel VARCHAR(32) NOT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (content_id),
  FOREIGN KEY (user_id) REFERENCES users(user_id) ON DELETE SET NULL,
  INDEX idx_user_created (user_id, created_at),
  INDEX idx_created_at (created_at),
  INDEX idx_dir_type (direction, content_type),
  INDEX idx_channel (channel)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE deleted_contents (
  deleted_content_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  original_content_id BIGINT UNSIGNED NOT NULL,
  user_id BIGINT UNSIGNED NULL,
  content_type ENUM('message','file','system') NOT NULL,
  direction ENUM('send','receive') NOT NULL,
  channel VARCHAR(32) NOT NULL,
  created_at TIMESTAMP NOT NULL,
  deleted_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (deleted_content_id),
  INDEX idx_original_id (original_content_id),
  INDEX idx_user_deleted (user_id, deleted_at),
  INDEX idx_deleted_at (deleted_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE messages (
  contents_id BIGINT UNSIGNED NOT NULL PRIMARY KEY,
  text_content TEXT NOT NULL,
  FOREIGN KEY (contents_id) REFERENCES contents(content_id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE deleted_messages (
  original_content_id BIGINT UNSIGNED NOT NULL,
  text_content TEXT NOT NULL,
  PRIMARY KEY (original_content_id),
  FOREIGN KEY (original_content_id) REFERENCES deleted_contents(original_content_id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE files (
  contents_id BIGINT UNSIGNED NOT NULL PRIMARY KEY,
  file_name VARCHAR(255) NOT NULL,
  file_size BIGINT UNSIGNED NULL,
  FOREIGN KEY (contents_id) REFERENCES contents(content_id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE deleted_files (
  original_content_id BIGINT UNSIGNED NOT NULL,
  file_name VARCHAR(255) NOT NULL,
  file_size BIGINT UNSIGNED NULL,
  PRIMARY KEY (original_content_id),
  FOREIGN KEY (original_content_id) REFERENCES deleted_contents(original_content_id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE shorthands (
  shorthand_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  user_id BIGINT UNSIGNED NOT NULL,
  key_name VARCHAR(255) NOT NULL,
  value_text TEXT NOT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (shorthand_id),
  FOREIGN KEY (user_id) REFERENCES users(user_id) ON DELETE CASCADE,
  UNIQUE KEY unique_user_key (user_id, key_name),
  INDEX idx_user (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE shorthand_updates (
  update_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  shorthand_id BIGINT UNSIGNED NOT NULL,
  old_value TEXT NOT NULL,
  v INT UNSIGNED NOT NULL DEFAULT 1,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (update_id),
  FOREIGN KEY (shorthand_id) REFERENCES shorthands(shorthand_id) ON DELETE CASCADE,
  INDEX idx_shorthand_v (shorthand_id, v),
  UNIQUE KEY unique_shorthand_version (shorthand_id, v)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE deleted_shorthands (
  deleted_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  source ENUM('shorthands','shorthand_updates') NOT NULL,
  shorthand_id BIGINT UNSIGNED NOT NULL,
  user_id BIGINT UNSIGNED NULL,
  key_name VARCHAR(255) NULL,
  value_text TEXT NOT NULL,
  v INT UNSIGNED NULL,
  deleted_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (deleted_id),
  INDEX idx_src_id_v (source, shorthand_id, v),
  INDEX idx_user (user_id),
  INDEX idx_key (key_name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

DROP TABLE IF EXISTS reserved_shorthands;

CREATE TABLE reserved_shorthands (
  res_shorthand_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  key VARCHAR(255) NOT NULL UNIQUE,
  purpose VARCHAR(500) NOT NULL,
  PRIMARY KEY (res_shorthand_id),
  INDEX idx_key (key)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

DELIMITER $$

DROP FUNCTION IF EXISTS db_get_content $$
CREATE FUNCTION db_get_content()
RETURNS TEXT
READS SQL DATA
DETERMINISTIC
BEGIN
  DECLARE result TEXT DEFAULT '';
  DECLARE done INT DEFAULT 0;
  DECLARE cid BIGINT;
  DECLARE ctype VARCHAR(32);
  DECLARE cur CURSOR FOR SELECT content_id, content_type FROM contents ORDER BY content_id ASC;
  DECLARE CONTINUE HANDLER FOR NOT FOUND SET done = 1;

  OPEN cur;
  read_loop: LOOP
    FETCH cur INTO cid, ctype;
    IF done THEN LEAVE read_loop; END IF;
    IF result = '' THEN
      SET result = CONCAT(cid, '|', ctype);
    ELSE
      SET result = CONCAT(result, ',', cid, '|', ctype);
    END IF;
  END LOOP;
  CLOSE cur;
  RETURN result;
END $$

DROP FUNCTION IF EXISTS create_user $$
CREATE FUNCTION create_user (
  p_username VARCHAR(50),
  p_password_hash VARCHAR(255),
  p_email VARCHAR(255),
  p_display_name VARCHAR(100)
) RETURNS BIGINT UNSIGNED
MODIFIES SQL DATA
DETERMINISTIC
BEGIN
  DECLARE v_user_id BIGINT UNSIGNED DEFAULT 0;
  DECLARE CONTINUE HANDLER FOR SQLEXCEPTION SET v_user_id = 0;
  IF p_username IS NULL OR p_username = '' OR p_password_hash IS NULL THEN RETURN 0; END IF;
  INSERT INTO users (username, password_hash, email, display_name)
  VALUES (p_username, p_password_hash, p_email, p_display_name);
  SET v_user_id = LAST_INSERT_ID();
  RETURN v_user_id;
END $$

DROP FUNCTION IF EXISTS verify_user $$
CREATE FUNCTION verify_user (
  p_username VARCHAR(50),
  p_password_hash VARCHAR(255)
) RETURNS BIGINT UNSIGNED
READS SQL DATA
BEGIN
  DECLARE v_user_id BIGINT UNSIGNED DEFAULT 0;
  SELECT user_id INTO v_user_id
  FROM users
  WHERE username COLLATE utf8mb4_unicode_ci = p_username COLLATE utf8mb4_unicode_ci
    AND password_hash COLLATE utf8mb4_unicode_ci = p_password_hash COLLATE utf8mb4_unicode_ci
    AND is_active = TRUE
  LIMIT 1;
  IF v_user_id IS NOT NULL AND v_user_id > 0 THEN
    UPDATE users SET last_login_at = CURRENT_TIMESTAMP WHERE user_id = v_user_id;
  END IF;
  RETURN IFNULL(v_user_id, 0);
END $$

DROP FUNCTION IF EXISTS log_contents $$
CREATE FUNCTION log_contents (
  p_user_id BIGINT UNSIGNED,
  p_type VARCHAR(16),
  p_direction VARCHAR(16),
  p_channel VARCHAR(32),
  p_text TEXT,
  p_file_name VARCHAR(255),
  p_file_size BIGINT UNSIGNED
) RETURNS BIGINT UNSIGNED
MODIFIES SQL DATA
DETERMINISTIC
BEGIN
  DECLARE v_contents_id BIGINT UNSIGNED DEFAULT 0;
  DECLARE CONTINUE HANDLER FOR SQLEXCEPTION SET v_contents_id = 0;
  IF p_type IS NULL OR p_direction IS NULL OR p_channel IS NULL THEN RETURN 0; END IF;
  IF p_type = 'message' AND (p_text IS NULL OR p_text = '') THEN RETURN 0; END IF;
  IF p_type = 'file' AND (p_file_name IS NULL OR p_file_name = '') THEN RETURN 0; END IF;
  INSERT INTO contents (user_id, content_type, direction, channel)
  VALUES (p_user_id, p_type, p_direction, p_channel);
  SET v_contents_id = LAST_INSERT_ID();
  IF p_type = 'message' THEN
    INSERT INTO messages (contents_id, text_content) VALUES (v_contents_id, p_text);
  ELSEIF p_type = 'file' THEN
    INSERT INTO files (contents_id, file_name, file_size) VALUES (v_contents_id, p_file_name, IFNULL(p_file_size, 0));
  ELSEIF p_type = 'system' THEN
    INSERT INTO messages (contents_id, text_content) VALUES (v_contents_id, IFNULL(p_text, ''));
  END IF;
  RETURN v_contents_id;
END $$

DROP FUNCTION IF EXISTS log_shorthand $$
CREATE FUNCTION log_shorthand (
  p_user_id BIGINT UNSIGNED,
  p_key VARCHAR(255),
  p_value TEXT
) RETURNS INT
DETERMINISTIC
MODIFIES SQL DATA
BEGIN
  DECLARE EXIT HANDLER FOR SQLEXCEPTION RETURN 0;
  IF p_user_id IS NULL OR p_key IS NULL OR p_key = '' THEN RETURN 0; END IF;
  INSERT INTO shorthands (user_id, key_name, value_text)
  VALUES (p_user_id, p_key, p_value)
  ON DUPLICATE KEY UPDATE value_text = p_value;
  RETURN 1;
END $$

DROP FUNCTION IF EXISTS db_get_shorthand $$
CREATE FUNCTION db_get_shorthand (
  p_user_id BIGINT UNSIGNED,
  p_key VARCHAR(255),
  p_v INT UNSIGNED
) RETURNS TEXT
READS SQL DATA
BEGIN
  DECLARE result_value TEXT DEFAULT NULL;
  DECLARE sh_id BIGINT UNSIGNED;
  SELECT shorthand_id, value_text INTO sh_id, result_value
  FROM shorthands
  WHERE user_id = p_user_id AND key_name = p_key;
  IF sh_id IS NULL THEN RETURN '0'; END IF;
  IF p_v = 0 THEN RETURN result_value; END IF;
  SELECT old_value INTO result_value
  FROM shorthand_updates
  WHERE shorthand_id = sh_id AND v = p_v;
  IF result_value IS NULL THEN RETURN '0'; END IF;
  RETURN result_value;
END $$

DROP FUNCTION IF EXISTS db_del_shorthand $$
CREATE FUNCTION db_del_shorthand (
  p_user_id BIGINT UNSIGNED,
  p_key VARCHAR(255)
) RETURNS INT
DETERMINISTIC
MODIFIES SQL DATA
BEGIN
  DECLARE sh_id BIGINT UNSIGNED DEFAULT NULL;
  DECLARE rv INT DEFAULT 0;
  DECLARE EXIT HANDLER FOR SQLEXCEPTION RETURN 0;
  SELECT shorthand_id INTO sh_id
  FROM shorthands
  WHERE user_id = p_user_id AND key_name = p_key
  LIMIT 1;
  IF sh_id IS NULL THEN RETURN 0; END IF;
  DELETE FROM shorthands WHERE shorthand_id = sh_id;
  SET rv = ROW_COUNT();
  IF rv > 0 THEN RETURN 1; END IF;
  RETURN 0;
END $$

DROP FUNCTION IF EXISTS db_del_content $$
CREATE FUNCTION db_del_content (
  p_user_id BIGINT UNSIGNED,
  p_content_id BIGINT UNSIGNED
) RETURNS INT
DETERMINISTIC
MODIFIES SQL DATA
BEGIN
  DECLARE rows_deleted INT DEFAULT 0;
  DECLARE EXIT HANDLER FOR SQLEXCEPTION RETURN 0;
  IF p_content_id = 0 THEN
    DELETE FROM contents WHERE user_id = p_user_id;
    SET rows_deleted = ROW_COUNT();
    IF rows_deleted > 0 THEN RETURN 1; ELSE RETURN 0; END IF;
  END IF;
  DELETE FROM contents WHERE content_id = p_content_id AND user_id = p_user_id;
  SET rows_deleted = ROW_COUNT();
  IF rows_deleted > 0 THEN RETURN 1; ELSE RETURN 0; END IF;
END $$

DROP FUNCTION IF EXISTS is_reserved $$
CREATE FUNCTION is_reserved (
  p_key VARCHAR(255)
) RETURNS INT
READS SQL DATA
DETERMINISTIC
BEGIN
  DECLARE v_count INT DEFAULT 0;
  DECLARE EXIT HANDLER FOR SQLEXCEPTION RETURN 0;
  SELECT COUNT(*) INTO v_count FROM reserved_shorthands WHERE `key` = p_key COLLATE utf8mb4_unicode_ci;
  IF v_count > 0 THEN RETURN 1; ELSE RETURN 0; END IF;
END $$

DROP FUNCTION IF EXISTS db_get_history $$
CREATE FUNCTION db_get_history (
  p_content_id BIGINT UNSIGNED,
  p_type VARCHAR(50)
) RETURNS JSON
READS SQL DATA
DETERMINISTIC
BEGIN
  DECLARE result_json JSON DEFAULT NULL;
  IF p_content_id IS NULL OR p_type IS NULL THEN
    RETURN JSON_OBJECT('error', 'Invalid parameters');
  END IF;
  IF p_type = 'message' OR p_type = 'system' THEN
    SELECT JSON_OBJECT(
      'content_id', p_content_id,
      'type', p_type,
      'text_content', COALESCE(m.text_content, '')
    ) INTO result_json
    FROM messages m
    WHERE m.contents_id = p_content_id
    LIMIT 1;
  ELSEIF p_type = 'file' THEN
    SELECT JSON_OBJECT(
      'content_id', p_content_id,
      'type', p_type,
      'file_name', COALESCE(f.file_name, ''),
      'file_size', COALESCE(f.file_size, 0)
    ) INTO result_json
    FROM files f
    WHERE f.contents_id = p_content_id
    LIMIT 1;
  ELSE
    RETURN JSON_OBJECT('error', 'Unknown type');
  END IF;
  IF result_json IS NULL THEN RETURN JSON_OBJECT('error', 'No data found'); END IF;
  RETURN result_json;
END $$

DELIMITER $$

DROP TRIGGER IF EXISTS del_content_trig $$
CREATE TRIGGER del_content_trig
BEFORE DELETE ON contents
FOR EACH ROW
BEGIN
  DECLARE msg_text TEXT DEFAULT NULL;
  DECLARE file_name_val VARCHAR(255) DEFAULT NULL;
  DECLARE file_size_val BIGINT UNSIGNED DEFAULT NULL;
  INSERT INTO deleted_contents (original_content_id, user_id, content_type, direction, channel, created_at)
  VALUES (OLD.content_id, OLD.user_id, OLD.content_type, OLD.direction, OLD.channel, OLD.created_at);
  IF OLD.content_type = 'message' THEN
    SELECT text_content INTO msg_text FROM messages WHERE contents_id = OLD.content_id;
    IF msg_text IS NOT NULL THEN
      INSERT INTO deleted_messages (original_content_id, text_content)
      VALUES (OLD.content_id, msg_text);
    END IF;
    DELETE FROM messages WHERE contents_id = OLD.content_id;
  ELSEIF OLD.content_type = 'file' THEN
    SELECT file_name, file_size INTO file_name_val, file_size_val FROM files WHERE contents_id = OLD.content_id;
    IF file_name_val IS NOT NULL THEN
      INSERT INTO deleted_files (original_content_id, file_name, file_size)
      VALUES (OLD.content_id, file_name_val, IFNULL(file_size_val, 0));
    END IF;
    DELETE FROM files WHERE contents_id = OLD.content_id;
  ELSEIF OLD.content_type = 'system' THEN
    SELECT text_content INTO msg_text FROM messages WHERE contents_id = OLD.content_id;
    IF msg_text IS NOT NULL THEN
      INSERT INTO deleted_messages (original_content_id, text_content)
      VALUES (OLD.content_id, msg_text);
    END IF;
    DELETE FROM messages WHERE contents_id = OLD.content_id;
  END IF;
END $$

DROP TRIGGER IF EXISTS shorthand_update $$
CREATE TRIGGER shorthand_update
BEFORE UPDATE ON shorthands
FOR EACH ROW
BEGIN
  DECLARE max_version INT UNSIGNED DEFAULT 0;
  IF OLD.value_text != NEW.value_text THEN
    SELECT IFNULL(MAX(v), 0) INTO max_version
    FROM shorthand_updates
    WHERE shorthand_id = OLD.shorthand_id;
    INSERT INTO shorthand_updates (shorthand_id, old_value, v)
    VALUES (OLD.shorthand_id, OLD.value_text, max_version + 1);
  END IF;
END $$

DROP TRIGGER IF EXISTS del_shorthands_trig $$
CREATE TRIGGER del_shorthands_trig
BEFORE DELETE ON shorthands
FOR EACH ROW
BEGIN
  INSERT INTO deleted_shorthands (source, shorthand_id, user_id, key_name, value_text, v)
  VALUES ('shorthands', OLD.shorthand_id, OLD.user_id, OLD.key_name, OLD.value_text, NULL);
END $$

DROP TRIGGER IF EXISTS del_shorthand_updates_trig $$
CREATE TRIGGER del_shorthand_updates_trig
BEFORE DELETE ON shorthand_updates
FOR EACH ROW
BEGIN
  DECLARE k VARCHAR(255);
  DECLARE u BIGINT UNSIGNED;
  SELECT key_name, user_id INTO k, u FROM shorthands WHERE shorthand_id = OLD.shorthand_id LIMIT 1;
  INSERT INTO deleted_shorthands (source, shorthand_id, user_id, key_name, value_text, v)
  VALUES ('shorthand_updates', OLD.shorthand_id, u, k, OLD.old_value, OLD.v);
END $$

DELIMITER ;