-- ---------------------------------------------------------------------------
-- mod-collection: account-wide mount and companion collections
--
-- The core's database updater applies this at startup because it belongs to an
-- enabled module: UpdateFetcher::ReceiveIncludedDirectories walks
-- modules/<name>/data/sql/db-characters. Nothing has to be applied by hand.
--
-- CREATE TABLE IF NOT EXISTS, never DROP: a MODULE file is re-applied whenever
-- its hash changes, and this table is the only record of what each account has
-- collected. The spells are taught to characters temporarily and never written
-- to character_spell, so dropping this table would not inconvenience the
-- updater - it would delete every collection on the realm.
--
-- Kind is 0 for a mount (SkillLineAbility line 777) and 1 for a companion
-- (line 778). It is stored rather than derived so the table can be read
-- without the DBC to hand.
-- ---------------------------------------------------------------------------

CREATE TABLE IF NOT EXISTS `account_collection` (
  `AccountId`  int unsigned     NOT NULL              COMMENT 'account.id',
  `SpellId`    int unsigned     NOT NULL              COMMENT 'the mount or companion spell',
  `Kind`       tinyint unsigned NOT NULL DEFAULT '0'  COMMENT '0 mount (skill 777), 1 companion (skill 778)',
  `FirstGuid`  int unsigned     NOT NULL DEFAULT '0'  COMMENT 'characters.guid that first collected it, for curiosity',
  `CollectedAt` timestamp       NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`AccountId`,`SpellId`),
  KEY `idx_spell` (`SpellId`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-collection: account-wide mounts and companions';
